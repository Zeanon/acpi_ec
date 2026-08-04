/*
 * This file is just an altered version of ec_sys.c in the Linux kernel.
 * I just modified it to make it work as an out-of-tree module and
 * to not use debugfs.
 *
 * Original copyright:
 * Copyright (C) 2010 SUSE Products GmbH/Novell
 * Author:
 *      Thomas Renninger <trenn@suse.de>
 */

// TODO: Add support for more than one EC controller.
#include <linux/acpi.h>
#include <linux/leds.h>
#include <linux/cdev.h>
#include <linux/device.h>
#include <linux/fs.h>
#include <linux/kernel.h>
#include <linux/module.h>
#include <linux/uaccess.h>
#include <linux/version.h>

MODULE_AUTHOR("Thomas Renninger <trenn@suse.de>");
MODULE_AUTHOR("Sayafdine Said <musikid@outlook.com>");
MODULE_AUTHOR("Vinzenz Hassert <thezeanon@gmail.com>");
MODULE_DESCRIPTION("ACPI EC access driver for MSI Prestige 16 Flip AI+ C3M");
MODULE_LICENSE("GPL");

static DEFINE_MUTEX(ec_set_bit_mutex);

#define EC_SPACE_SIZE 256

#define MSI_EC_FW_VERSION_ADDRESS 0xa0
#define MSI_EC_FW_VERSION_LENGTH  12

#define MUTE_LED_BIT 0x02
#define MUTE_LED_ADDRESS 0x2D
#define MIC_MUTE_LED_ADDRESS 0x2C

extern int ec_read(u8 addr, u8 *val);
extern int ec_write(u8 addr, u8 val);
extern struct acpi_ec *first_ec;

static dev_t first_dev;
static struct cdev c_dev;
static struct class *dev_class;

static ssize_t acpi_ec_read(struct file *f, char __user *buf, size_t count,
                            loff_t *off) {
  unsigned int size = EC_SPACE_SIZE;
  loff_t init_off = *off;
  int err = 0;

  if (*off >= size)
    return 0;

  if (*off + count >= size) {
    size -= *off;
    count = size;
  } else
    size = count;

  while (size) {
    u8 byte_read;
    err = ec_read(*off, &byte_read);
    if (err)
      return err;

    if (put_user(byte_read, buf + *off - init_off)) {
      if (*off - init_off)
        return *off - init_off; /* partial read */
      return -EFAULT;
    }

    *off += 1;
    size--;
  }
  return count;
}

static ssize_t acpi_ec_write(struct file *f, const char __user *buf,
                             size_t count, loff_t *off) {
  unsigned int size = count;
  loff_t init_off = *off;
  int err = 0;

  if (*off >= EC_SPACE_SIZE)
    return 0;

  if (*off + count >= EC_SPACE_SIZE) {
    size = EC_SPACE_SIZE - *off;
    count = size;
  }

  while (size) {
    u8 byte_write;
    if (get_user(byte_write, buf + *off - init_off)) {
      if (*off - init_off)
        return *off - init_off; /* partial write */
      return -EFAULT;
    }
    err = ec_write(*off, byte_write);
    if (err)
      return err;

    *off += 1;
    size--;
  }
  return count;
}

static int ec_read_seq(u8 addr, u8 *buf, u8 len)
{
	int result;
	for (u8 i = 0; i < len; i++) {
		result = ec_read(addr + i, buf + i);
		if (result < 0)
			return result;
	}
	return 0;
}

static inline int ec_set_bit(u8 addr, u8 bit, bool value)
{
	int result;
	u8 stored;

	mutex_lock(&ec_set_bit_mutex);
	result = ec_read(addr, &stored);
	if (result < 0)
		goto unlock;

	if (value)
		stored |= bit;
	else
		stored &= ~bit;

	result = ec_write(addr, stored);

unlock:
	mutex_unlock(&ec_set_bit_mutex);
	return result;
}

static int micmute_led_sysfs_set(struct led_classdev *led_cdev,
				                         enum led_brightness brightness)
{
	int result;

	result = ec_set_bit(MIC_MUTE_LED_ADDRESS, MUTE_LED_BIT, brightness);

	if (result < 0)
		return result;

	return 0;
}

static int mute_led_sysfs_set(struct led_classdev *led_cdev, 
                              enum led_brightness brightness)
{
	int result;

	result = ec_set_bit(MUTE_LED_ADDRESS, MUTE_LED_BIT, brightness);

	if (result < 0)
		return result;

	return 0;
}

static const struct file_operations fops = {
    .owner = THIS_MODULE,
    .open = simple_open,
    .read = acpi_ec_read,
    .write = acpi_ec_write,
    .llseek = default_llseek,
};

static struct led_classdev micmute_led_cdev = {
	.name = "platform::micmute",
	.max_brightness = 1,
	.brightness_set_blocking = &micmute_led_sysfs_set,
	.default_trigger = "audio-micmute",
};

static struct led_classdev mute_led_cdev = {
	.name = "platform::mute",
	.max_brightness = 1,
	.brightness_set_blocking = &mute_led_sysfs_set,
	.default_trigger = "audio-mute",
};

static int acpi_ec_create_dev(void) {
  int err = -1;

  if ((err = alloc_chrdev_region(&first_dev, 0, 1, "ec")) < 0) {
    printk(KERN_ERR "acpi_ec: Failed to allocate a char_dev region\n");
    return err;
  }

#if LINUX_VERSION_CODE < KERNEL_VERSION(6, 4, 0)
  if (IS_ERR(dev_class = class_create(THIS_MODULE, "chardev")))
#else
  if (IS_ERR(dev_class = class_create("chardev")))
#endif
  {
    printk(KERN_ERR "acpi_ec: Failed to create a class\n");
    err = -1;
    goto error;
  }

  if (IS_ERR(device_create(dev_class, NULL, first_dev, NULL, "ec"))) {
    printk(KERN_ERR "acpi_ec: Failed to create a device\n");
    err = -1;
    class_destroy(dev_class);
    goto error;
  }

  cdev_init(&c_dev, &fops);

  if ((err = cdev_add(&c_dev, first_dev, 1)) < 0) {
    printk(KERN_ERR "acpi_ec: Failed to add a device\n");
    device_destroy(dev_class, first_dev);
    class_destroy(dev_class);
    goto error;
  }

  char ec_version[MSI_EC_FW_VERSION_LENGTH + 1];
  memset(ec_version, 0, MSI_EC_FW_VERSION_LENGTH + 1);
  if ((err = ec_read_seq(MSI_EC_FW_VERSION_ADDRESS, ec_version, MSI_EC_FW_VERSION_LENGTH)) < 0) {
    printk(KERN_ERR "acpi_ec: Failed to get firmware version\n");
    goto error;
  }

  printk(KERN_INFO "acpi_ec: ec version '%s'", ec_version);
  if (strcmp(ec_version, "2622EMS1.112") == 0) {
    printk(KERN_INFO "acpi_ec: adding mute indicator leds");
    // register the mute and mic mute leds
    if ((err = led_classdev_register(NULL, &micmute_led_cdev)) < 0) {
      printk(KERN_ERR "acpi_ec: Failed to add mic mute led device\n");
      led_classdev_unregister(&micmute_led_cdev);
      goto error;
    }

    if ((err = led_classdev_register(NULL, &mute_led_cdev)) < 0) {
      printk(KERN_ERR "acpi_ec: Failed to add mute led device\n");
      led_classdev_unregister(&mute_led_cdev);
      goto error;
    }
  } else {
    printk(KERN_INFO "acpi_ec: ec version '%s' not supported for mute indicator leds", ec_version);
  }

  return 0;

error:
  unregister_chrdev_region(first_dev, 1);
  return err;
}

static int __init acpi_ec_init(void) {
  if (first_ec)
    return acpi_ec_create_dev();
  else
    return -1;
}

static void __exit acpi_ec_exit(void) {
  cdev_del(&c_dev);
  device_destroy(dev_class, first_dev);
  class_destroy(dev_class);
  led_classdev_unregister(&micmute_led_cdev);
  led_classdev_unregister(&mute_led_cdev);
  unregister_chrdev_region(first_dev, 1);
}

module_init(acpi_ec_init);
module_exit(acpi_ec_exit);
