/**
 * @file aesdchar.c
 * @brief AESD character device driver implementation
 *
 * Based on "scull" device driver example from Linux Device Drivers.
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h>
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "aesdchar.h"
#include "aesd-circular-buffer.h"
#include "aesd_ioctl.h"

int aesd_major = 0; // dynamic major
int aesd_minor = 0;

MODULE_AUTHOR(""); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

/* -------------------------------------------------------------------------
 * Public API implementations
 * ----------------------------------------------------------------------*/
int aesd_open(struct inode *inode, struct file *filp)
{
    struct aesd_dev *device_ptr;

    PDEBUG("open");

    device_ptr = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = device_ptr;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *user_buf, size_t count,
                  loff_t *f_pos)
{
    struct aesd_dev *device_ptr = filp->private_data;
    struct aesd_buffer_entry *entry_ptr;
    char *temp_buf;
    int read_size;
    size_t entry_offset;

    PDEBUG("read %zu bytes at offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&device_ptr->lock))
        return -ERESTARTSYS;

    entry_ptr = aesd_circular_buffer_find_entry_offset_for_fpos(
        &device_ptr->cbuffer, *f_pos, &entry_offset);

    if (!entry_ptr) {
        mutex_unlock(&device_ptr->lock);
        return 0;
    }

    read_size = entry_ptr->size - entry_offset;
    if (count < read_size)
        read_size = count;

    temp_buf = kmalloc(read_size, GFP_KERNEL);
    if (!temp_buf) {
        mutex_unlock(&device_ptr->lock);
        return -ENOMEM;
    }

    memcpy(temp_buf, entry_ptr->buffptr + entry_offset, read_size);
    if (copy_to_user(user_buf, temp_buf, read_size))
        read_size = -EFAULT;

    kfree(temp_buf);
    *f_pos += read_size;

    mutex_unlock(&device_ptr->lock);
    return read_size;
}

ssize_t aesd_write(struct file *filp, const char __user *user_buf,
                   size_t count, loff_t *f_pos)
{
    struct aesd_dev *device_ptr = filp->private_data;
    struct aesd_buffer_entry new_entry;

    char *temp_user_buf;
    bool is_end_command;

    PDEBUG("write %zu bytes at offset %lld", count, *f_pos);

    if (mutex_lock_interruptible(&device_ptr->lock))
        return -ERESTARTSYS;

    temp_user_buf = kmalloc(count, GFP_KERNEL);
    if (!temp_user_buf) {
        mutex_unlock(&device_ptr->lock);
        return -ENOMEM;
    }

    if (copy_from_user(temp_user_buf, user_buf, count)) {
        kfree(temp_user_buf);
        mutex_unlock(&device_ptr->lock);
        return -EFAULT;
    }

    is_end_command = (temp_user_buf[count - 1] == '\n');

    if (is_end_command) {
        /* Merge with temporary buffer if necessary */
        if (device_ptr->tmp_buff.size > 0) {
            new_entry.size = device_ptr->tmp_buff.size + count;
            new_entry.buffptr = kmalloc(new_entry.size, GFP_KERNEL);
            memcpy(new_entry.buffptr, device_ptr->tmp_buff.buffptr, device_ptr->tmp_buff.size);
            memcpy(new_entry.buffptr + device_ptr->tmp_buff.size, temp_user_buf, count);

            kfree(device_ptr->tmp_buff.buffptr);
            device_ptr->tmp_buff.size = 0;

            kfree(temp_user_buf);
        } else {
            new_entry.size = count;
            new_entry.buffptr = temp_user_buf;
        }

        /* Add entry to circular buffer */
        if (device_ptr->cbuffer.full) {
            kfree(device_ptr->cbuffer.entry[device_ptr->cbuffer.in_offs].buffptr);
        }
        aesd_circular_buffer_add_entry(&device_ptr->cbuffer, &new_entry);

    } else {
        /* No newline: merge with tmp_buff */
        if (device_ptr->tmp_buff.size > 0) {
            char *merged_buf = kmalloc(device_ptr->tmp_buff.size + count, GFP_KERNEL);
            memcpy(merged_buf, device_ptr->tmp_buff.buffptr, device_ptr->tmp_buff.size);
            memcpy(merged_buf + device_ptr->tmp_buff.size, temp_user_buf, count);

            kfree(device_ptr->tmp_buff.buffptr);
            device_ptr->tmp_buff.buffptr = merged_buf;
            device_ptr->tmp_buff.size += count;

            kfree(temp_user_buf);
        } else {
            device_ptr->tmp_buff.buffptr = temp_user_buf;
            device_ptr->tmp_buff.size = count;
        }
    }

    mutex_unlock(&device_ptr->lock);
    return count;
}

loff_t aesd_llseek(struct file *filp, loff_t offset, int whence)
{
    struct aesd_dev *device_ptr = filp->private_data;
    loff_t new_pos;
    loff_t total_size = 0;
    uint8_t idx;

    if (mutex_lock_interruptible(&device_ptr->lock))
        return -ERESTARTSYS;

    for (idx = device_ptr->cbuffer.out_offs;
         (idx != device_ptr->cbuffer.in_offs) ||
         (device_ptr->cbuffer.full && total_size == 0);
         idx = (idx + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {
        total_size += device_ptr->cbuffer.entry[idx].size;
    }

    switch (whence) {
    case SEEK_SET:
        new_pos = offset;
        break;
    case SEEK_CUR:
        new_pos = filp->f_pos + offset;
        break;
    case SEEK_END:
        new_pos = total_size + offset;
        break;
    default:
        mutex_unlock(&device_ptr->lock);
        return -EINVAL;
    }

    if (new_pos < 0) {
        mutex_unlock(&device_ptr->lock);
        return -EINVAL;
    }

    filp->f_pos = new_pos;
    mutex_unlock(&device_ptr->lock);

    return new_pos;
}

long aesd_modify_foffset(struct file *filp, uint32_t write_cmd,
                         uint32_t write_cmd_offset)
{
    struct aesd_dev *device_ptr = filp->private_data;
    int i, buffer_index;
    uint8_t target_index = -1;
    size_t offset_accumulator = 0;

    if (mutex_lock_interruptible(&device_ptr->lock))
        return -ERESTARTSYS;

    for (i = 0, buffer_index = device_ptr->cbuffer.out_offs;
         (buffer_index != device_ptr->cbuffer.in_offs) ||
         (i == 0 && device_ptr->cbuffer.full);
         i++, buffer_index = (buffer_index + 1) % AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED) {

        if (i == write_cmd) {
            target_index = buffer_index;
            break;
        } else {
            offset_accumulator += device_ptr->cbuffer.entry[buffer_index].size;
        }
    }

    if (target_index == -1 || write_cmd_offset >= device_ptr->cbuffer.entry[target_index].size) {
        mutex_unlock(&device_ptr->lock);
        return EINVAL;
    }

    loff_t f_offset = offset_accumulator + write_cmd_offset;
    filp->f_pos = f_offset;

    mutex_unlock(&device_ptr->lock);
    return f_offset;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg)
{
    long retval;

    switch (cmd) {
    case AESDCHAR_IOCSEEKTO: {
        struct aesd_seekto seek_info;
        if (copy_from_user(&seek_info, (const void __user *)arg, sizeof(struct aesd_seekto)))
            retval = -EFAULT;
        else
            retval = aesd_modify_foffset(filp, seek_info.write_cmd, seek_info.write_cmd_offset);
        break;
    }
    default:
        retval = -EINVAL;
    }

    return retval;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
    .llseek = aesd_llseek,
    .unlocked_ioctl = aesd_ioctl,
};

/* -------------------------------------------------------------------------
 * Static helper functions
 * ----------------------------------------------------------------------*/
static int aesd_cdev_setup(struct aesd_dev *device_ptr)
{
    int err;
    dev_t dev_no = MKDEV(aesd_major, aesd_minor);

    cdev_init(&device_ptr->cdev, &aesd_fops);
    device_ptr->cdev.owner = THIS_MODULE;
    device_ptr->cdev.ops = &aesd_fops;

    err = cdev_add(&device_ptr->cdev, dev_no, 1);
    if (err)
        printk(KERN_ERR "Error %d adding aesd cdev\n", err);

    return err;
}

/* -------------------------------------------------------------------------
 * Module init / exit
 * ----------------------------------------------------------------------*/
int aesd_init_module(void)
{
    dev_t dev_no = 0;
    int result;

    result = alloc_chrdev_region(&dev_no, aesd_minor, 1, "aesdchar");
    aesd_major = MAJOR(dev_no);
    if (result < 0) {
        printk(KERN_WARNING "Cannot allocate major %d\n", aesd_major);
        return result;
    }

    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    aesd_circular_buffer_init(&aesd_device.cbuffer);
    mutex_init(&aesd_device.lock);

    result = aesd_cdev_setup(&aesd_device);
    if (result)
        unregister_chrdev_region(dev_no, 1);

    return result;
}

void aesd_cleanup_module(void)
{
    dev_t dev_no = MKDEV(aesd_major, aesd_minor);
    struct aesd_buffer_entry *entry_ptr;
    uint8_t idx;

    cdev_del(&aesd_device.cdev);

    AESD_CIRCULAR_BUFFER_FOREACH(entry_ptr, &aesd_device.cbuffer, idx) {
        kfree(entry_ptr->buffptr);
    }

    unregister_chrdev_region(dev_no, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
