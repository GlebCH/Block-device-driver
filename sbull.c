#include <linux/module.h>
#include <linux/moduleparam.h>
#include <linux/stat.h>
#include <linux/init.h>
#include <linux/device.h>
#include <scsi/scsi_device.h>

#include <linux/sched.h>
#include <linux/kernel.h>	/* printk() */
#include <linux/slab.h>		/* kmalloc() */
#include <linux/fs.h>		/* everything... */
#include <linux/errno.h>	/* error codes */
#include <linux/types.h>	/* size_t */
#include <linux/fcntl.h>	/* O_ACCMODE */
#include <linux/hdreg.h>	/* HDIO_GETGEO */
#include <linux/kdev_t.h>
#include <linux/vmalloc.h>
#include <linux/genhd.h>
#include <linux/blkdev.h>
#include <linux/buffer_head.h>	/* invalidate_bdev */
#include <linux/bio.h>



/* Create the major number for the disk.
 * Major nubmer identifies the driver
 * associated with the device.
 *
 * It works in pairs with the minor number.
 */
static int sbull_major;

/* Initialize the minor number.
 * The disk will not be partitioned.
 */
#define SBULL_MINOR	1


/* The size of the sector that the kernel processes */
#define KERNEL_SECTOR_SIZE	512


/* Device name */
static char *sbull_name = "sbull";
module_param(sbull_name, charp, S_IRUGO);

/* Device size in MB */
static unsigned int size = 100;
module_param(size, uint, S_IRUGO);

/* size(MB) * 1024 * 1024 -> size(bytes)
 * size(bytes) / 512 -> nsectors
 */
#define NSECTORS ((size)*2048)


/*
 * The internal representation of our device
 */
struct sbull_dev {
	int size;			/* Device size in bytes */
	u8 *data;			/* The data aray. u8 - 8 bytes */
	short users;			/* How many users */
	spinlock_t lock;		/* For mutual exclusion */
	struct request_queue *queue;	/* For mutual exclusion */
	struct gendisk *gd;		/* The gendisk structure */

	struct device_driver sbull_driver;	/* For represent the driver in
						 * sysfs. And adding it to bus.
						 */
	struct device sbull_device;		/* For represent the device in
						 * sysfs. And adding it to bus.
						 */
};
static struct sbull_dev *my_device;


/*
 * Handle an I/O request
 */
static void sbull_transfer(struct sbull_dev *dev, unsigned long sector,
		unsigned long nsect, char *buffer, int write)
{
	unsigned long offset = sector * KERNEL_SECTOR_SIZE;
	unsigned long nbytes = nsect * KERNEL_SECTOR_SIZE;

	if ((offset + nbytes) > dev->size) {
		printk(KERN_WARNING "Beyound-end write (%ld %ld)\n",
				offset, nbytes);
		return;
	}
	if (write)
		memcpy(dev->data + offset, buffer, nbytes);
	else
		memcpy(buffer, dev->data + offset, nbytes);
}

/*
 * Transfer a single BIO
 */
static int sbull_xfer_bio(struct sbull_dev *dev, struct bio *bio)
{
	int i;
	struct bio_vec *bvec;
	sector_t sector = bio->bi_sector;

	/* With kmap we get virtual addresses of bio */
	bio_for_each_segment(bvec, bio, i) {
		char *buffer = __bio_kmap_atomic(bio, i, KM_USER0);

		sbull_transfer(dev, sector,
				(bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE),
				buffer, bio_data_dir(bio) == WRITE);
		sector += (bio_cur_bytes(bio) / KERNEL_SECTOR_SIZE);

		__bio_kunmap_atomic(bio, KM_USER0);
	}
	return 0;
}

/*
 * Transfer a full request
 */
static int sbull_xfer_request(struct sbull_dev *dev, struct request *req)
{
	struct bio *bio;
	int nbytes = 0;

	/* When passing through the list,
	 * we count the number of bytes read.
	 *
	 * This is necessary for sbull_full_request.
	 */
	__rq_for_each_bio(bio, req) {
		sbull_xfer_bio(dev, bio);
		nbytes += bio->bi_size;
	}

	return nbytes;
}

/*
 * Handles request queue
 */
static void sbull_full_request(struct request_queue *q)
{
	struct request *req;
	int bytes_xferred;
	struct sbull_dev *dev = q->queuedata;


	req = blk_fetch_request(q);
	while (req != NULL) {
		if (req->cmd_type != REQ_TYPE_FS) {
			printk(KERN_INFO "Skip non-fs request\n");

			__blk_end_request_all(req, -EIO);
			req = blk_fetch_request(q);
			continue;
		}

		bytes_xferred = sbull_xfer_request(dev, req);

		/* If all bytes in request have been transferred
		 * reuest is terminated, otherwise we are
		 * working whith the same request.
		 */
		if (!__blk_end_request(req, 0, bytes_xferred))
			req = blk_fetch_request(q);
	}
}


/* sbull_open is called when the disk is opened.
 * Increase the number of users.
 */
static int sbull_open(struct block_device *bdev, fmode_t mode)
{
	struct sbull_dev *dev = bdev->bd_disk->private_data;

	spin_lock(&dev->lock);
	dev->users++;
	spin_unlock(&dev->lock);
	return 0;
}

/* sbull_release is calles whe the disk is closed.
 * Reduse the number of users.
 */
static void sbull_release(struct gendisk *disk, fmode_t mode)
{
	struct sbull_dev *dev = disk->private_data;

	spin_lock(&dev->lock);
	dev->users--;
	spin_unlock(&dev->lock);
	return;
}


/* Provide information about the geometry of the device.
 * Even if our device is virtual
 * this may be needed by some utilities.
 *
 * for example: fdsik
 */
int sbull_ioctl(struct block_device *bdev,
		fmode_t mode,
		unsigned int cmd, unsigned long arg)
{
	long size;
	struct hd_geometry geo;
	struct sbull_dev *dev = bdev->bd_disk->private_data;

	switch (cmd) {
	case HDIO_GETGEO:
		size = dev->size;
		geo.cylinders = (size & ~0x3f) >> 6;
		geo.heads = 4;
		geo.sectors = 16;
		geo.start = 4;
		if (copy_to_user((void __user *) arg, &geo, sizeof(geo)))
			return -EFAULT;
		return 0;
	}

	return -ENOTTY;
}


/*
 * The device operations structure
 */
static const struct block_device_operations sbull_ops = {
	.owner		= THIS_MODULE,
	.open		= sbull_open,
	.release	= sbull_release,
	.ioctl		= sbull_ioctl
};


/*
 * Method show for device attribute
 */
static ssize_t sbull_show(struct device *dev,
	       struct device_attribute *attr, char *buf)
{
	return sprintf(buf, "read-only file");
}

/*
 * Declare attribute
 */
static DEVICE_ATTR(sbull_attr, S_IRUGO, sbull_show, NULL);

/* Device attributes must be created
 * before the KOBJ_ADD uevent is generated.
 * The only way to realize that is by defining an attribute group.
 */
static struct attribute *sbull_attrs[] = {
	&dev_attr_sbull_attr.attr,
	NULL
};

static struct attribute_group sbull_attr_group = {
	.attrs = sbull_attrs
};

static const struct attribute_group *sbull_attr_groups[] = {
	&sbull_attr_group,
	NULL
};


/* release function for the device.
 * If is not defined, kernel will
 * throw a warning.
 */
static void device_release(struct device *dev)
{
	printk(KERN_INFO "release device");
}

extern struct bus_type scsi_bus_type;

/*
 * Register the driver in sysfs
 */
static int reg_driver(struct device_driver *ddev)
{
	ddev->name = "sbull";
	ddev->bus = &scsi_bus_type;
	ddev->owner = THIS_MODULE;

	return driver_register(ddev);
}

/*
 * Register the device in sysfs
 */
static int reg_device(struct device *dev, struct sbull_dev *sbull_dev)
{
	dev->init_name = sbull_name;
	dev->bus = &scsi_bus_type;

	dev_set_drvdata(dev, sbull_dev);
	dev->groups = sbull_attr_groups;

	dev->driver = &(sbull_dev->sbull_driver);
	dev->release = device_release;

	return device_register(dev);
}

/*
 * Setup up our devce
 */
static void setup_device(struct sbull_dev *dev)
{
	/*
	 * Get some memory
	 */
	memset(dev, 0, sizeof(struct sbull_dev));
	dev->size = NSECTORS*KERNEL_SECTOR_SIZE;
	dev->data = vmalloc(dev->size);
	if (dev->data == NULL) {
		printk(KERN_WARNING "vmalloc failure.\n");
		return;
	}
	spin_lock_init(&dev->lock);

	/*
	 * The I/O queue
	 */
	dev->queue = blk_init_queue(sbull_full_request, &dev->lock);
	if (dev->queue == NULL)
		goto out_vfree;
	blk_queue_logical_block_size(dev->queue, KERNEL_SECTOR_SIZE);
	dev->queue->queuedata = dev;
	/*
	 * gendisk structure
	 */
	dev->gd = alloc_disk(SBULL_MINOR);
	if (!dev->gd) {
		printk(KERN_WARNING "alloc_disk failure\n");
		goto out_vfree;
	}
	dev->gd->major = sbull_major;
	dev->gd->first_minor = SBULL_MINOR;
	dev->gd->fops = &sbull_ops;
	dev->gd->queue = dev->queue;
	dev->gd->private_data = dev;
	snprintf(dev->gd->disk_name, 32, sbull_name);
	set_capacity(dev->gd, NSECTORS);
	add_disk(dev->gd);

	return;

out_vfree:
	if (dev->data)
		vfree(dev->data);
}

static int __init sbull_init(void)
{
	int ret;

	sbull_major = register_blkdev(sbull_major, sbull_name);
	if (sbull_major <= 0) {
		printk(KERN_WARNING "sbull: unable to get major number\n");
		return -EBUSY;
	}

	my_device = kmalloc(sizeof(struct sbull_dev), GFP_KERNEL);
	if (my_device == NULL)
		goto out_unregister;
	setup_device(my_device);
	printk(KERN_INFO "sbull: device is created");

	ret = reg_driver(&(my_device->sbull_driver));
	if (!ret)
		printk(KERN_INFO "sbull: add driver on scsi bus");
	else
		printk(KERN_WARNING "sbull: coudn't add driver on scsi bus");


	ret = reg_device(&(my_device->sbull_device), my_device);
	if (!ret)
		printk(KERN_INFO "sbull: add device on scsi bus");
	else
		printk(KERN_WARNING "sbull: coudn't add device on scsi bus");

	printk(KERN_INFO "sbull: the module is loaded");
	return 0;

out_unregister:
	unregister_blkdev(sbull_major, sbull_name);

	return -ENOMEM;
}

static void __exit sbull_exit(void)
{
	if (my_device->gd) {
		del_gendisk(my_device->gd);
		put_disk(my_device->gd);
	}
	if (my_device->queue)
		blk_cleanup_queue(my_device->queue);
	if (my_device->data)
		vfree(my_device->data);

	unregister_blkdev(sbull_major, sbull_name);
	driver_unregister(&(my_device->sbull_driver));
	device_unregister(&(my_device->sbull_device));
	kfree(my_device);
}


module_init(sbull_init);
module_exit(sbull_exit);

MODULE_LICENSE("GPL");
MODULE_AUTHOR("Gleb Chesnokov");
