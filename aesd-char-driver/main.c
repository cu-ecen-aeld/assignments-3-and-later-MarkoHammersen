 
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
#include <linux/slab.h>
#include <asm/uaccess.h>
#include "aesdchar.h"
#include "aesd-circular-buffer.h"
#include "aesd_ioctl.h"

int aesd_major =   0; // use dynamic major
int aesd_minor =   0;

MODULE_AUTHOR(""); /** TODO: fill in your name **/
MODULE_LICENSE("Dual BSD/GPL");

struct aesd_dev aesd_device;

int aesd_open(struct inode *inode, struct file *filp)
{
    PDEBUG("open");
    /**
     * TODO: handle open
     */
    struct aesd_dev* dev;

    dev = container_of(inode->i_cdev, struct aesd_dev, cdev);
    filp->private_data = dev;

    return 0;
}

int aesd_release(struct inode *inode, struct file *filp)
{
    PDEBUG("release");
    /**
     * TODO: handle release
     */
    return 0;
}

ssize_t aesd_read(struct file *filp, char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = 0;
    PDEBUG("read %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle read
     */
    struct aesd_dev* adev = filp->private_data;
    char* out_buff;
    int out_size;
    struct aesd_buffer_entry* aux_entry;
    size_t aux_offset;

    if (mutex_lock_interruptible(&adev->lock))
         return -ERESTARTSYS;

    aux_entry = aesd_circular_buffer_find_entry_offset_for_fpos(&adev->cbuffer, *f_pos, &aux_offset);
    if(aux_entry == NULL){
        mutex_unlock(&adev->lock);
        return 0;
    }

    out_size = aux_entry->size - aux_offset;
    if(count < out_size)
        out_size = count;
    out_buff = kmalloc(out_size, GFP_KERNEL);

    memcpy(out_buff, aux_entry->buffptr + aux_offset, out_size);
    *f_pos = *f_pos + out_size;

    copy_to_user(buf, out_buff, out_size);
    //kfree(out_buff);

    mutex_unlock(&adev->lock);
    return out_size;
}

ssize_t aesd_write(struct file *filp, const char __user *buf, size_t count,
                loff_t *f_pos)
{
    ssize_t retval = -ENOMEM;
    PDEBUG("write %zu bytes with offset %lld",count,*f_pos);
    /**
     * TODO: handle write
     */
    struct aesd_dev* adev = filp->private_data;
    struct aesd_buffer_entry def_buff;


    if (mutex_lock_interruptible(&adev->lock))
         return -ERESTARTSYS;

    char* aux_buff = kmalloc(count, GFP_KERNEL);
    copy_from_user(aux_buff, buf, count);


    bool end_command = aux_buff[count-1] == '\n';
    if(end_command){
        if(adev->tmp_buff.size){    // Have to concat
            def_buff.size = adev->tmp_buff.size + count;
            def_buff.buffptr = kmalloc(def_buff.size, GFP_KERNEL);

            memcpy(def_buff.buffptr, adev->tmp_buff.buffptr, adev->tmp_buff.size);             
            memcpy(def_buff.buffptr + adev->tmp_buff.size, aux_buff, count);

            kfree(adev->tmp_buff.buffptr);
            adev->tmp_buff.size = 0;

            kfree(aux_buff);
        }else{      // Not having to concat
            def_buff.size = count;
            def_buff.buffptr = aux_buff;
        }

        //Insert entry
        if(adev->cbuffer.full){   // Free memory of last entry before overwriting
            kfree(adev->cbuffer.entry[adev->cbuffer.in_offs].buffptr);
        }
        aesd_circular_buffer_add_entry(&adev->cbuffer, &def_buff);
    }else{
        if(adev->tmp_buff.size){    // Have to concat
            char* tmp_buff2 = kmalloc(adev->tmp_buff.size + count, GFP_KERNEL);

            memcpy(tmp_buff2, adev->tmp_buff.buffptr, adev->tmp_buff.size);
            memcpy(tmp_buff2 + adev->tmp_buff.size, aux_buff, count);

            kfree(adev->tmp_buff.buffptr);
            adev->tmp_buff.buffptr = tmp_buff2;
            adev->tmp_buff.size = adev->tmp_buff.size + count;

            kfree(aux_buff);
        }else{  // Not having to concat
            adev->tmp_buff.size = count;
            adev->tmp_buff.buffptr = aux_buff;
        }

    }

    mutex_unlock(&adev->lock);
    return count;
}

loff_t aesd_llseek(struct file *filp, loff_t off, int whence){
    struct aesd_dev* adev = filp->private_data;
    loff_t new_pos;
    loff_t total_size = 0;
    uint8_t i;

    if (mutex_lock_interruptible(&adev->lock))
        return -ERESTARTSYS;

    for(i = adev->cbuffer.out_offs; (i != adev->cbuffer.in_offs) || ((adev->cbuffer.full)&&(total_size == 0)); i = (i+1)%AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED)
        total_size += adev->cbuffer.entry[i].size;


    switch(whence){
        case SEEK_SET:
            new_pos = off;
            break;
        case SEEK_CUR:
            new_pos = filp->f_pos + off;
            break;
        case SEEK_END:
            new_pos = total_size + off;
            break;
        default:
            mutex_unlock(&adev->lock);
            return -EINVAL;
    }
    if(new_pos < 0) {
        mutex_unlock(&adev->lock);
        return -EINVAL;
    }

    filp->f_pos = new_pos;
    mutex_unlock(&adev->lock);

    return new_pos;
}

long aesd_modify_foffset(struct file *filp, uint32_t write_cmd, uint32_t write_cmd_offset){
    struct aesd_dev *adev = filp->private_data;

    uint8_t cmd_found = -1;
    size_t current_size = 0;
    int i, index;

    if (mutex_lock_interruptible(&adev->lock))
        return -ERESTARTSYS;

    for(i = 0, index = adev->cbuffer.out_offs; (index != adev->cbuffer.in_offs) || (i == 0 && adev->cbuffer.full) ;i++, index = (index+1)%AESDCHAR_MAX_WRITE_OPERATIONS_SUPPORTED){     
        if(i == write_cmd){
            cmd_found = index;
            break;                      // or add ` && cmd_found == -1`    in for boolean
        }else
            current_size += adev->cbuffer.entry[index].size;
    } 

    if(cmd_found == -1 || write_cmd_offset >= adev->cbuffer.entry[cmd_found].size){
        mutex_unlock(&adev->lock);
        return EINVAL;
    }


    loff_t f_offset = current_size + write_cmd_offset;
    filp->f_pos = f_offset;
    mutex_unlock(&adev->lock);

    return f_offset;
}

long aesd_ioctl(struct file *filp, unsigned int cmd, unsigned long arg){
    long out;

    switch(cmd){
        case AESDCHAR_IOCSEEKTO:
            struct aesd_seekto seekto;
            if(copy_from_user(&seekto, (const void __user *)arg, sizeof(struct aesd_seekto)))
                out = EFAULT;
            else
                out = aesd_modify_foffset(filp, seekto.write_cmd, seekto.write_cmd_offset);
            break;
        default:
            out = EINVAL;
    }
    return out;
}

struct file_operations aesd_fops = {
    .owner =    THIS_MODULE,
    .read =     aesd_read,
    .write =    aesd_write,
    .open =     aesd_open,
    .release =  aesd_release,
    .llseek =   aesd_llseek,
    .unlocked_ioctl =    aesd_ioctl,
};

static int aesd_setup_cdev(struct aesd_dev *dev)
{
    int err, devno = MKDEV(aesd_major, aesd_minor);

    cdev_init(&dev->cdev, &aesd_fops);
    dev->cdev.owner = THIS_MODULE;
    dev->cdev.ops = &aesd_fops;
    err = cdev_add (&dev->cdev, devno, 1);
    if (err) {
        printk(KERN_ERR "Error %d adding aesd cdev", err);
    }
    return err;
}



int aesd_init_module(void)
{
    dev_t dev = 0;
    int result;
    result = alloc_chrdev_region(&dev, aesd_minor, 1,
            "aesdchar");
    aesd_major = MAJOR(dev);
    if (result < 0) {
        printk(KERN_WARNING "Can't get major %d\n", aesd_major);
        return result;
    }
    memset(&aesd_device,0,sizeof(struct aesd_dev));

    /**
     * TODO: initialize the AESD specific portion of the device
     */
    aesd_circular_buffer_init(&aesd_device.cbuffer);
    mutex_init(&aesd_device.lock);

    result = aesd_setup_cdev(&aesd_device);

    if( result ) {
        unregister_chrdev_region(dev, 1);
    }
    return result;

}

void aesd_cleanup_module(void)
{
    dev_t devno = MKDEV(aesd_major, aesd_minor);

    cdev_del(&aesd_device.cdev);

    /**
     * TODO: cleanup AESD specific poritions here as necessary
     */

    uint8_t index;
    struct aesd_buffer_entry *entry;
    AESD_CIRCULAR_BUFFER_FOREACH(entry, &aesd_device.cbuffer, index){
        kfree(entry->buffptr);
    }

    unregister_chrdev_region(devno, 1);
}



module_init(aesd_init_module);
module_exit(aesd_cleanup_module);
