/*
 * ABACOM USB relayboard driver - 1.0
 *
 */

#include <linux/module.h>
#include <linux/init.h>
#include <linux/usb.h>
#include <linux/kernel.h>
#include <linux/uaccess.h>
#include <linux/slab.h>
#include <linux/errno.h>
#include <linux/kref.h>
#include <linux/mutex.h>

/*
 *	Metainformation
 */

MODULE_AUTHOR( "Heiko Finzel" );
MODULE_LICENSE( "GPL" );
MODULE_DESCRIPTION( "Driver for the ABACOM usb relayboard." );

/*
 * Constants
 */

#define DEBUG
/* Define vendor & product match IDs */
#define USB_RELAYBOARD_VENDOR_ID	0x1a86 
#define USB_RELAYBOARD_PRODUCT_ID	0x5512
#define USB_RELAYBOARD_MINOR_BASE	0
#define RELAY_READ_FREQ_MAX			(HZ / 2) //500ms
#define RELAY_CMD(b)				{0xa1,0x6a,0x1f,0x00,0x10,b,0x3f,0x00,0x00,0x00,0x00}
#define RELAY_CMD_LENGTH			11

/*
 * Our struct definitions
 */

/* Structure definition to hold all device (instance) specific information */
struct usb_relayboard {
	/* The usb_device for this device */
	struct usb_device	*udev;		
	/* The usb_interface for this device */
	struct usb_interface	*interface;		
	/* Reference counter, the driver itself is "1", each open increases, each close 
		decreases ... if count is 0, free this object */
	struct kref			kref;
	/* Mutex to keep track of actions pending on this device */
	struct semaphore	mutex;		
	/* 1 byte holding the state of each relay on the board (this one can't be
		queried from the device itself) */
	__u8				relay_states; 
};
#define to_relayboard_dev(d) container_of(d, struct usb_relayboard, kref)

/* A struct to hold some file instance specific information */
struct file_additions {
	struct usb_relayboard *device;
	unsigned long last_call; /* value in jiffies */
};

/*
 * Define Functions (see below)
 */

static int relayboard_probe(struct usb_interface *interface,
		      const struct usb_device_id *id);
static void relayboard_disconnect(struct usb_interface *interface);
static int relayboard_open(struct inode *inode, struct file *file);
static int relayboard_close(struct inode *inode, struct file *file);
static ssize_t relayboard_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos);
static ssize_t relayboard_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos);
static int relayboard_send_status(struct usb_relayboard *dev, __u8 status);
static int send_relay_cmd(struct usb_relayboard *dev, char cmd);

/*
 * Descriptors
 */

/* Device table for this driver */
static const struct usb_device_id relayboard_device_table[] = {
	{ USB_DEVICE( USB_RELAYBOARD_VENDOR_ID, USB_RELAYBOARD_PRODUCT_ID ) },
	{ 0,}				
};
MODULE_DEVICE_TABLE( usb, relayboard_device_table );

/* Specify the driver itself */
static struct usb_driver relayboard_driver = {
	.name =		"abacomrelay",
	.id_table =	relayboard_device_table,
	.probe =	relayboard_probe,
	.disconnect =	relayboard_disconnect,
};

/* Systemcalls provided by this driver */
static const struct file_operations relayboard_fops = {
	.owner =	THIS_MODULE,
	.open =		relayboard_open,
	.release =	relayboard_close,
	.read =		relayboard_read,
	.write =	relayboard_write,
};

static char *relay_devnode(struct device *dev, mode_t *mode)
{
    return kasprintf(GFP_KERNEL, "usb/%s", dev_name(dev));
}

/* Descriptor to let the system know the name of the device, the supported
	systemcalls and the minorID to start with */
static struct usb_class_driver relayboard_descriptor = {
	.name =		"relayboard%d",
	.devnode = relay_devnode, 
	.fops =		&relayboard_fops,
	.minor_base =	USB_RELAYBOARD_MINOR_BASE,
};

/*
 * Driver handling
 */

static int __init usb_relayboard_init(void)
{
	int result;
	/* Don't accidentally provide the usb_class_driver descriptior here or 
		you will freeze the system on driver loading ! ^^ */
	if(result = usb_register(&relayboard_driver) ) {
		printk( KERN_ERR "abacomrelay: Driver registration failed, error %d.\n",
			result);
	}
	return result;
}

static void __exit usb_relayboard_exit(void)
{
	usb_deregister(&relayboard_driver);
}

module_init(usb_relayboard_init);
module_exit(usb_relayboard_exit);

/*
 * Device handling
 */

/* Free the memory of our device structure */
static void relayboard_free(struct kref *kref)
{
	struct usb_relayboard *dev = to_relayboard_dev(kref);
	usb_put_dev(dev->udev);
	kfree(dev);
}

/* If a device (matching relayboard_device_table) gets connected... */
static int relayboard_probe(struct usb_interface *interface,
		      const struct usb_device_id *id)
{
	struct usb_device *device;
	int result;
	device = interface_to_usbdev(interface);
	/* FIXME! As idVendor actually refers to "QinHeng Electronics"(chip vendor)
		I'm not sure if the combination of idVendor and idProduct is really 
		unique for this board, so I will also test some other conditions here */
	if(device->descriptor.bNumConfigurations==1
		/* Using this because 96mA seems to be a rather unusual value, which is
			perfect for matching a device that has no unique identifier :) */
		&& device->config[0].desc.bMaxPower==48
		&& device->config[0].desc.bNumInterfaces==1
		/* Finally test for the number of endpoints, cur_altsetting will refer
			to the correct setting of this device as we have only one */
		&& device->config[0].interface[0]->cur_altsetting->desc.bNumEndpoints==3) {
	/* Initialize our own device structure to hold additional information */
		struct usb_relayboard *dev;
		/* Zero the allocated memory, note that this also sets relay_states to
			"all relays off", which is the board's natural behavior */
		dev = kzalloc(sizeof(*dev), GFP_KERNEL);
		if (!dev) {
			printk( KERN_ERR "abacomrelay: Error creating device structure. Out of memory.\n");
			return -ENOMEM;
		}
		kref_init(&dev->kref);
		sema_init(&dev->mutex, 1);
		dev->udev = usb_get_dev(device);
		dev->interface = interface;
		usb_set_intfdata(interface, dev);
	/* Register the device to be used with this driver */
		if( result = usb_register_dev( interface, &relayboard_descriptor ) ) {
			printk( KERN_ERR "abacomrelay: Device registration failed, error %d.\n", result);
			usb_set_intfdata(interface, NULL);
			kref_put(&dev->kref, relayboard_free);
		}
		return result;
	}
	return -ENODEV;
}

/* If a board gets disconnected or the driver is removed while the board is 
	still connected... */
static void relayboard_disconnect(struct usb_interface *interface)
{
	struct usb_relayboard *dev;
	dev = usb_get_intfdata(interface);
	/* Wait for pending operations and deregister device 
		Waiting is not really necessary for the device, as it disables all 
		relays on disconnect anyway, but it's meant as a protection against 
		kernel-oops */
	down( &dev->mutex );
	usb_set_intfdata(interface, NULL);
	usb_deregister_dev(interface, &relayboard_descriptor);
	dev->interface = NULL;
	up( &dev->mutex );
	/* Free memory used by our structure */
	kref_put(&dev->kref, relayboard_free);
}

/*
 * Systemcall API
 */

static int relayboard_open(struct inode *inode, struct file *file)
{
	struct usb_relayboard *dev;
	struct usb_interface *interface;
	struct file_additions *infos;
	int minor;
	minor = iminor(inode);
	interface = usb_find_interface(&relayboard_driver, minor);
	if (!interface) {
		printk( KERN_ERR "abacomrelay: Can't find device with minor %d for open call.\n", minor);
		return -ENODEV;
	}
	dev = usb_get_intfdata(interface);
	if (!dev) {
		return -ENODEV;
	}
	kref_get(&dev->kref);
	/* Save our object in the private structure of the instance */
	infos = kzalloc(sizeof(*infos), GFP_KERNEL);
	if (!infos) {
		printk( KERN_ERR "abacomrelay: Error creating instance structure. Out of memory.\n");
		return -ENOMEM;
	}
    infos->device = dev;
	file->private_data = infos;
	return 0;
}

static int relayboard_close(struct inode *inode, struct file *file)
{
	struct file_additions *infos;
	struct usb_relayboard *dev;
	infos = file->private_data;
	dev = infos->device;
	if (dev == NULL) {
		return -ENODEV;
	}
	kref_put(&dev->kref, relayboard_free);
	kfree(infos);
	return 0;
}

static ssize_t relayboard_write(struct file *file, const char *user_buffer,
			  size_t count, loff_t *ppos)
{
	struct file_additions *infos;
    struct usb_relayboard *dev;
	char *local_buffer;
	__u8 user_data;
	infos = file->private_data;
	dev = infos->device;
	local_buffer = kzalloc(count, GFP_KERNEL);
    if (!local_buffer) {
        return -ENOMEM;
    }
    if (copy_from_user(local_buffer, user_buffer, count)) {
		kfree(local_buffer);
        return -EFAULT;
    }
	/* Use only the lower 8 bits of parsed value */
	user_data = (__u8) simple_strtoul(local_buffer,NULL,10);
	kfree(local_buffer);
	down( &dev->mutex );
    if (relayboard_send_status(dev, user_data)) {
		count = -EFAULT;
	}
	up( &dev->mutex );
	return count;
}

/* Actual communication with the device and saving the status */
static int relayboard_send_status(struct usb_relayboard *dev, __u8 status) {
	__u8 mask;
	/* Send the command frame */
	if (send_relay_cmd(dev, 0x00)) goto error;
	for (mask = 128; mask > 0; mask >>= 1) {
		if (status & mask) {
			/* Send "relay on" */
			if (send_relay_cmd(dev, 0x20)) goto error;
			if (send_relay_cmd(dev, 0x28)) goto error;
			if (send_relay_cmd(dev, 0x20)) goto error;
		} else {
			/* Send "relay off" */
			if (send_relay_cmd(dev, 0x00)) goto error;
			if (send_relay_cmd(dev, 0x08)) goto error;
			if (send_relay_cmd(dev, 0x00)) goto error;
		}
	}
	/* End the command frame */
	if (send_relay_cmd(dev, 0x00)) goto error;
	if (send_relay_cmd(dev, 0x01)) goto error;
	/* Remember the status */
	dev->relay_states = status;
	return 0;
error:
	/* FIXME! Don't know if a "reset" is needed here, I will investigate that 
		later */
	return -EFAULT;
}

static int send_relay_cmd(struct usb_relayboard *dev, char cmd) {
	char transfer_bytes[] = RELAY_CMD(cmd);
	int actual_length;
	if (usb_bulk_msg( dev->udev, usb_sndbulkpipe(dev->udev, 2) , transfer_bytes, RELAY_CMD_LENGTH, 
		&actual_length, HZ*2 )) {
		return -EFAULT;
	}
	return actual_length != RELAY_CMD_LENGTH;
}

/* This actually doesn't read from the device but "from the driver" */
static ssize_t relayboard_read(struct file *file, char *buffer, size_t count,
			 loff_t *ppos)
{
	struct file_additions *infos;
	struct usb_relayboard *dev;
	unsigned long timestamp = jiffies;
	unsigned long time_passed;
	char state_str[5];
	/* We won't read if count is too small, because reading one char at a
		time doesn't make sense, the state could change till next call 
		We need 3 chars (max. byte = "255") + newline */
	if (count < 4) {
		return 0;
	}
	infos = file->private_data;
	/* If last read call from this handler was less then RELAY_READ_FREQ_MAX 
		ms ago, return "end of file", this is to provite functionality for
		"cat" and allow programs to call "read" multiple times without 
		the need to re-open the device in one solution */
	if (infos->last_call > timestamp) {
		time_passed	= ULONG_MAX - infos->last_call + timestamp + 1;
	} else {
		time_passed = timestamp - infos->last_call;
	}
	if (time_passed < RELAY_READ_FREQ_MAX) {
		return 0;
	}
	dev = infos->device;
	down( &dev->mutex );
	snprintf( state_str, sizeof(state_str), "%d\n", dev->relay_states );
	count = strlen(state_str);
	count -= copy_to_user(buffer,state_str,count);
	infos->last_call = timestamp;
	up( &dev->mutex );
	return count;
}
