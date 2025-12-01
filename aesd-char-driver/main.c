/**
 * @file aesdchar.c
 * @brief Functions and data related to the AESD char driver implementation
 *
 * Based on the implementation of the "scull" device driver, found in
 * Linux Device Drivers example code.
 *
 * @author Dan Walkes
 * @date 2019-10-22
 * @copyright Copyright (c) 2019
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/printk.h>
#include <linux/types.h>
#include <linux/cdev.h>
#include <linux/fs.h> // file_operations
#include "aesd-circular-buffer.h"
#include "aesdchar.h"

static int aesd_major = 0; // use dynamic major
static int aesd_minor = 0;

MODULE_AUTHOR("Marko Hammersen"); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

static struct aesd_dev aesd_device;

static int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev *dev; /* device information */
    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev; /* for other methods */

    return 0;
}

static int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     * Nothing to do, since the data pointed to in filp->private_data structure was allocated in init_module (not open()).
       Therefore should be freed in module_exit().
     */
    return 0;
}

static ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                  loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld", count, *f_pos);
    /**
     * TODO: handle read
     */
    struct aesd_dev *dev = filp->private_data;
    
    if (mutex_lock_interruptible(&dev->lock))
    {
        PDEBUG("failed acquire lock");
        return -ERESTARTSYS;
    }  

    size_t entry_offset_byte_rtn;
    struct aesd_buffer_entry *entry = aesd_circular_buffer_find_entry_offset_for_fpos(&dev->cbuf, (size_t)*f_pos, &entry_offset_byte_rtn);
    if(entry == NULL)
    {
        mutex_unlock(&dev->lock);
        return 0;
    }
    size_t out_size = entry->size - entry_offset_byte_rtn;
    *f_pos = *f_pos + out_size;
    retval = out_size;
    if(0 < copy_to_user(buf, &entry->buffptr[entry_offset_byte_rtn], out_size))
    {
        PDEBUG("failed to copy to user");
        retval = 0;
    }    
    mutex_unlock(&dev->lock);
    
    return retval;
}

static ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                   loff_t *f_pos)
{
    PDEBUG("write %zu bytes with offset %lld", count, *f_pos);

    ssize_t retval = -ENOMEM;
    struct aesd_dev *dev = filp->private_data;

    if (mutex_lock_interruptible(&dev->lock))
    {
        return -ERESTARTSYS;
    }  

    char *new_buffer = (char*)kmalloc(dev->current_write_entry.size + count, GFP_KERNEL);
    if(new_buffer == NULL)
    {
        PDEBUG("failed to kmalloc");
        mutex_unlock(&dev->lock);
        return retval;
    }

    if(dev->current_write_entry.buffptr != NULL) 
    {
        memcpy(new_buffer, dev->current_write_entry.buffptr, dev->current_write_entry.size);
        kfree(dev->current_write_entry.buffptr);
        
    }
    dev->current_write_entry.buffptr = new_buffer;
    new_buffer = NULL;

    if(copy_from_user(&dev->current_write_entry.buffptr[dev->current_write_entry.size], buf, count))
    {
        PDEBUG("copy_from_user failed");
        if (dev->current_write_entry.buffptr != NULL)
        {
            kfree(dev->current_write_entry.buffptr);
        }
        dev->current_write_entry.buffptr = NULL;
        dev->current_write_entry.size = 0;
        mutex_unlock(&dev->lock);    
        return -EFAULT;
    }
    dev->current_write_entry.size += count;

    if(dev->current_write_entry.buffptr[dev->current_write_entry.size - 1] == '\n')
    {
        if(dev->cbuf.full)
        {
            kfree(&dev->cbuf.entry[dev->cbuf.in_offs].buffptr);
        }

        char *temp = kmalloc(dev->current_write_entry.size + 1, GFP_KERNEL);
        if(temp != NULL)
        {
            memcpy(temp, dev->current_write_entry.buffptr, dev->current_write_entry.size);
            temp[dev->current_write_entry.size] = '\0';
            PDEBUG("write to cb: %s", temp);
            kfree(temp);
        }

        // write to circular buffer        
        aesd_circular_buffer_add_entry(&dev->cbuf, &dev->current_write_entry);
        dev->current_write_entry.buffptr = NULL;
        dev->current_write_entry.size = 0;
    }

    mutex_unlock(&dev->lock);
    
    retval = count;    
    return retval;
}

struct file_operations aesd_fops = {
    .owner = THIS_MODULE,
    .read = aesd_read,
    .write = aesd_write,
    .open = aesd_open,
    .release = aesd_release,
};

static int aesd_setup_cdev(struct aesd_dev * dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add(&dev->cdev, devno, 1);
    if (err)
    {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}

static int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
                                    "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0)
    {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device, 0, sizeof(struct aesd_dev));

    aesd_device.current_write_entry.buffptr = NULL;
    aesd_device.current_write_entry.size = 0;

    aesd_circular_buffer_init(&aesd_device.cbuf);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if (result)
    {
        unregister_chrdev_region(dev, 1);
    }
    return result;
}

static void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */
    uint32_t index;
    struct aesd_circular_buffer buffer;
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry,&buffer,index) 
    {
        kfree(entry->buffptr);
    }    

    unregister_chrdev_region(devno, 1);
}

module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
