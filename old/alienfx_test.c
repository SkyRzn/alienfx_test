/*
 * MSI GT683R led driver
 *
 * Copyright (c) 2014 Janne Kanniainen <janne.kanniainen@gmail.com>
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of
 * the License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/kernel.h>
#include <linux/leds.h>
#include <linux/module.h>
#include <linux/delay.h>

#include <linux/random.h> //FIXME

#include "hid-ids.h"

#define GT683R_BUFFER_SIZE                      8

/*
 * GT683R_LED_OFF: all LEDs are off
 * GT683R_LED_AUDIO: LEDs brightness depends on sound level
 * GT683R_LED_BREATHING: LEDs brightness varies at human breathing rate
 * GT683R_LED_NORMAL: LEDs are fully on when enabled
 */
enum gt683r_led_mode {
	GT683R_LED_OFF = 0,
	GT683R_LED_AUDIO = 2,
	GT683R_LED_BREATHING = 3,
	GT683R_LED_NORMAL = 5
};

enum gt683r_panels {
	GT683R_LED_BACK = 0,
	GT683R_LED_SIDE = 1,
	GT683R_LED_FRONT = 2,
	GT683R_LED_COUNT,
};

static const char * const gt683r_panel_names[] = {
	"back",
	"side",
	"front",
};

struct gt683r_led {
	struct hid_device *hdev;
	struct led_classdev led_devs[GT683R_LED_COUNT];
	struct mutex lock;
	struct work_struct work;
	enum led_brightness brightnesses[GT683R_LED_COUNT];
	enum gt683r_led_mode mode;
};

static const struct hid_device_id gt683r_led_id[] = {
	{ HID_USB_DEVICE(0x187c, 0x0522) },
	{ }
};

static void gt683r_brightness_set(struct led_classdev *led_cdev,
								  enum led_brightness brightness)
{
	int i;
	struct device *dev = led_cdev->dev->parent;
	struct hid_device *hdev = container_of(dev, struct hid_device, dev);
	struct gt683r_led *led = hid_get_drvdata(hdev);

	for (i = 0; i < GT683R_LED_COUNT; i++) {
		if (led_cdev == &led->led_devs[i])
			break;
	}

	if (i < GT683R_LED_COUNT) {
		led->brightnesses[i] = brightness;
		schedule_work(&led->work);
	}
}

static ssize_t mode_show(struct device *dev,
						 struct device_attribute *attr,
						 char *buf)
{
	u8 sysfs_mode;
	struct hid_device *hdev = container_of(dev->parent,
										   struct hid_device, dev);
	struct gt683r_led *led = hid_get_drvdata(hdev);

	if (led->mode == GT683R_LED_NORMAL)
		sysfs_mode = 0;
	else if (led->mode == GT683R_LED_AUDIO)
		sysfs_mode = 1;
	else
		sysfs_mode = 2;

	return scnprintf(buf, PAGE_SIZE, "%u\n", sysfs_mode);
}

static ssize_t mode_store(struct device *dev,
						  struct device_attribute *attr,
						  const char *buf, size_t count)
{
	u8 sysfs_mode;
	struct hid_device *hdev = container_of(dev->parent,
										   struct hid_device, dev);
	struct gt683r_led *led = hid_get_drvdata(hdev);


	if (kstrtou8(buf, 10, &sysfs_mode) || sysfs_mode > 2)
		return -EINVAL;

	mutex_lock(&led->lock);

	if (sysfs_mode == 0)
		led->mode = GT683R_LED_NORMAL;
	else if (sysfs_mode == 1)
		led->mode = GT683R_LED_AUDIO;
	else
		led->mode = GT683R_LED_BREATHING;

	mutex_unlock(&led->lock);
	schedule_work(&led->work);

	return count;
}



static int snd_(struct gt683r_led *led, u8 *msg, int cnt)
{
	int ret, i;
	u8 *buffer;

	buffer = kzalloc(16, GFP_KERNEL);
	for (i=0; i<9; i++)
		buffer[i] = msg[i];
	ret = hid_hw_raw_request(led->hdev, buffer[0], buffer, cnt, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
	if (ret != cnt) {
		hid_err(led->hdev, "failed to send set report request: %i\n", ret);
		if (ret < 0)
			return ret;
		return -EIO;
	}

	kfree(buffer);

	return 0;
}

static int get_(struct gt683r_led *led)
{
	int ret, i;
	u8 *buffer;

	buffer = kzalloc(16, GFP_KERNEL);

	ret = hid_hw_raw_request(led->hdev, 1, buffer, 9, HID_INPUT_REPORT, HID_REQ_GET_REPORT);
	printk(KERN_ALERT "!!!!!!!!!!!!!!!!!! %d\n", ret);
// 	if (ret != 8) {
// 		hid_err(led->hdev, "failed to send set report request: %i\n", ret);
// 		if (ret < 0)
// 			return ret;
// 		return -EIO;
// 	}

	printk(KERN_ALERT "<<< %.2x %.2x %.2x %.2x %.2x %.2x %.2x %.2x\n", buffer[0],buffer[1],buffer[2],buffer[3],
		   buffer[4],buffer[5],buffer[6],buffer[7]);


	kfree(buffer);

	return 0;
}

// 02 07 04 00 00 00 00 00 00 reset RESET_ALL_LIGHTS_ON
// 02 06 00 00 00 00 00 00 00 get status
// 02 08 05 00 00 00 00 00 00 save next 05
// 02 03 01 00 60 00 f0 00 00 set color 01 (reg=006000, rg=f0 b=00)
// 02 08 05 00 00 00 00 00 00 save next 05
// 02 04 00 00 00 00 00 00 00 loop
// 02 09 00 00 00 00 00 00 00 save
//
// 02 05 00 00 00 00 00 00 00 tr exec



static int gt683r_led_snd_msg(struct gt683r_led *led, u8 *msg)
{
	int ret;

	int br = led->brightnesses[0];

	u8 c_reset[] = {0x02, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	u8 c_reset_off[] = {0x02, 0x07, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	u8 c_save_next[] = {0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	u8 c_save[] = {0x02, 0x09, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	u8 c_c1[] = {0x02, 0x03, 0x01, 0x00, 0x60, 0x00, 0xff, 0xf0, 0x00};
	u8 c_loop[] = {0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	u8 c_c2[] = {0x02, 0x03, 0x02, 0x00, 0x09, 0x61, 0x0f, 0x00, 0x00};
	u8 c_c3[] = {0x02, 0x03, 0x01, 0x00, 0x00, 0x01, 0xff, 0x00, 0x00};
	u8 c_c4[] = {0x02, 0x03, 0x02, 0x00, 0x00, 0x60, 0xff, 0x00, 0x00};
	u8 c_tr_exec[] = {0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
	u8 c_get_stat[] = {0x02, 0x06, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

// 	br >>= 2;
// 	u8 r = (br & 3) * 4;
// 	br >>= 2;
// 	u8 g = (br & 3) * 4;
// 	br >>= 2;
// 	u8 b = ((br & 3) * 4) << 4;
// 	u8 rg = ((r << 4) & 0xf0) | (b & 0x0f);

// 	u8 r = (br & 7) * 2;
// 	br >>= 3;
// 	u8 g = (br & 7) * 2;
// 	br >>= 3;
// 	u8 b = ((br & 3) * 4) << 4;
// 	u8 rg = ((r << 4) & 0xf0) | (g & 0x0f);

	int i;

// 	0x0001 - keyboard
// 	0x0020 - left speaker
// 	0x0040 - right speaker
// 	0x0100 - logo
// 	0x0800 - media-bar

	snd_(led, c_reset, 9);
	msleep(1);

	u8 rnd;
	int lol[] = {0x1, 0x20, 0x40, 0x100, 0x800};
	for (i=0; i<2; i++) {

// 		get_random_bytes(&rnd, 1);
		int reg = lol[rnd & 3];
		reg = 0x961;
		u8 reg2 = (reg >> 8) & 0xff;
		u8 reg3 = reg & 0xff;
		c_c2[2] = i;

		c_c2[4] = reg2;
		c_c2[5] = reg3;
		get_random_bytes(&rnd, 1);
		c_c2[6] = rnd;
		get_random_bytes(&rnd, 1);
		c_c2[7] = rnd & 0xf0;
		snd_(led, c_c2, 9);
		snd_(led, c_loop, 9);
	}
	snd_(led, c_tr_exec,9);


// 	snd_(led, c_reset, 3);
// 	printk(KERN_ALERT "1\n");
// 	snd_(led, c_get_stat);
// 	get_(led);
// 	printk(KERN_ALERT "2\n");
// 	snd_(led, c_save_next);
	// 	printk(KERN_ALERT "3\n");
// 	snd_(led, c_c1);
// 	printk(KERN_ALERT "4\n");
// 	snd_(led, c_save_next);
// 	printk(KERN_ALERT "5\n");
// 	snd_(led, c_loop);
// 	printk(KERN_ALERT "6\n");
// 	snd_(led, c_save_next);
// 	printk(KERN_ALERT "7\n");
// 	snd_(led, c_c2);
// 	printk(KERN_ALERT "8\n");
// 	snd_(led, c_save_next);
// 	printk(KERN_ALERT "9\n");
// 	snd_(led, c_loop);
// 	printk(KERN_ALERT "10\n");

// 	snd_(led, c_reset);
// 	snd_(led, c_get_stat);
// 	snd_(led, c_c3);
// 	snd_(led, c_loop);
// 	snd_(led, c_c4);
// 	snd_(led, c_loop);

// 	snd_(led, c_tr_exec);


	//      ret = hid_hw_raw_request(led->hdev, msg[0], msg, GT683R_BUFFER_SIZE,
	//                                                       HID_FEATURE_REPORT, HID_REQ_SET_REPORT);
	//      if (ret != GT683R_BUFFER_SIZE) {
	//              hid_err(led->hdev,
	//                              "failed to send set report request: %i\n", ret);
	//              if (ret < 0)
	//                      return ret;
	//              return -EIO;
	//      }

	return 0;
}

static int gt683r_leds_set(struct gt683r_led *led, u8 leds)
{
	int ret;
	u8 *buffer;

	buffer = kzalloc(GT683R_BUFFER_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer[0] = 0x01;
	buffer[1] = 0x02;
	buffer[2] = 0x30;
	buffer[3] = leds;
	ret = gt683r_led_snd_msg(led, buffer);

	kfree(buffer);
	return ret;
}

static int gt683r_mode_set(struct gt683r_led *led, u8 mode)
{
	int ret;
	u8 *buffer;

	buffer = kzalloc(GT683R_BUFFER_SIZE, GFP_KERNEL);
	if (!buffer)
		return -ENOMEM;

	buffer[0] = 0x01;
	buffer[1] = 0x02;
	buffer[2] = 0x20;
	buffer[3] = mode;
	buffer[4] = 0x01;
	ret = gt683r_led_snd_msg(led, buffer);

	kfree(buffer);
	return ret;
}

static void gt683r_led_work(struct work_struct *work)
{
	int i;
	u8 leds = 0;
	u8 mode;
	struct gt683r_led *led = container_of(work, struct gt683r_led, work);

	mutex_lock(&led->lock);

	for (i = 0; i < GT683R_LED_COUNT; i++) {
		if (led->brightnesses[i])
			leds |= BIT(i);
	}

	if (gt683r_leds_set(led, leds))
		goto fail;

	if (leds)
		mode = led->mode;
	else
		mode = GT683R_LED_OFF;

	gt683r_mode_set(led, mode);
	fail:
	mutex_unlock(&led->lock);
}

static DEVICE_ATTR_RW(mode);

static struct attribute *gt683r_led_attrs[] = {
	&dev_attr_mode.attr,
	NULL
};

static const struct attribute_group gt683r_led_group = {
	.name = "gt683r",
	.attrs = gt683r_led_attrs,
};

static const struct attribute_group *gt683r_led_groups[] = {
	&gt683r_led_group,
	NULL
};

static int gt683r_led_probe(struct hid_device *hdev,
							const struct hid_device_id *id)
{
	int i;
	int ret;
	int name_sz;
	char *name;
	struct gt683r_led *led;

	led = devm_kzalloc(&hdev->dev, sizeof(*led), GFP_KERNEL);
	if (!led)
		return -ENOMEM;

	mutex_init(&led->lock);
	INIT_WORK(&led->work, gt683r_led_work);

	led->mode = GT683R_LED_NORMAL;
	led->hdev = hdev;
	hid_set_drvdata(hdev, led);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parsing failed\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret;
	}

	for (i = 0; i < GT683R_LED_COUNT; i++) {
		name_sz = strlen(dev_name(&hdev->dev)) +
		strlen(gt683r_panel_names[i]) + 3;

		name = devm_kzalloc(&hdev->dev, name_sz, GFP_KERNEL);
		if (!name) {
			ret = -ENOMEM;
			goto fail;
		}

		snprintf(name, name_sz, "%s::%s",
				 dev_name(&hdev->dev), gt683r_panel_names[i]);
		led->led_devs[i].name = name;
		led->led_devs[i].max_brightness = 255;
		led->led_devs[i].brightness_set = gt683r_brightness_set;
		led->led_devs[i].groups = gt683r_led_groups;

		ret = led_classdev_register(&hdev->dev, &led->led_devs[i]);
		if (ret) {
			hid_err(hdev, "could not register led device\n");
			goto fail;
		}
	}

	return 0;

	fail:
	for (i = i - 1; i >= 0; i--)
		led_classdev_unregister(&led->led_devs[i]);
	hid_hw_stop(hdev);
	return ret;
}

static void gt683r_led_remove(struct hid_device *hdev)
{
	int i;
	struct gt683r_led *led = hid_get_drvdata(hdev);

	for (i = 0; i < GT683R_LED_COUNT; i++)
		led_classdev_unregister(&led->led_devs[i]);
	flush_work(&led->work);
	hid_hw_stop(hdev);
}

static int rraw(struct hid_device * hdev, struct hid_report *rep, u8 *raw_data, int size)
{
	printk(KERN_ALERT "EVENT\n");
	return 1;
}
static int *event(struct hid_device *hdev, struct hid_field *field, struct hid_usage *usage, __s32 value)
{
	printk(KERN_ALERT "REPORT!!\n");
	return 1;
}

static struct hid_driver gt683r_led_driver = {
	.probe = gt683r_led_probe,
	.remove = gt683r_led_remove,
	.name = "aaa",
	.id_table = gt683r_led_id,
	.raw_event = rraw,
	.event = event,
};

module_hid_driver(gt683r_led_driver);

MODULE_AUTHOR("Janne Kanniainen");
MODULE_DESCRIPTION("MSI GT683R led driver");
MODULE_LICENSE("GPL");
