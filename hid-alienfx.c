/*
 * AlienFX led driver
 *
 * Copyright (c) 2015 Alexandr Ivanov <alexandr.sky@gmail.com>
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

#include "hid-ids.h"


struct alienfx_led;

struct alienfx_dev {
	struct hid_device *hdev;
	struct alienfx_led *leds;
	struct mutex lock;
	struct work_struct work;
};

enum led_state {
	LED_STATE_INIT = 0,
	LED_STATE_NEW_VALUE
};

struct alienfx_led {
	const char *name;
	unsigned int id;
	struct led_classdev classdev;
	int color;
	enum led_state state;
	struct alienfx_dev *dev;
};

struct alienfx_led m11x_leds[] = {
	{.name = "keyboard", .id = 0x0001},
	{.name = "speaker-left", .id = 0x0020},
	{.name = "speaker-right", .id = 0x0040},
	{.name = "logo", .id = 0x0100},
	{.name = "media-bar", .id = 0x0800},
	{.name = "power-button", .id = 0x2000},
	{.name = "power-button-eyes", .id = 0x4000},
	{.name = "power-reset-state", .id = 0x8000},
	{.name = "all-but-power", .id = 0x0961},
	{.name = "all", .id = 0xe961},
	{ }
};

u8 CMD_RESET[]		= {0x02, 0x07, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
u8 CMD_TR_EXEC[]	= {0x02, 0x05, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
u8 CMD_LOOP[]		= {0x02, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};
u8 CMD_SET_COL[]	= {0x02, 0x03, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00};

static int alienfx_send_cmd(struct alienfx_dev *dev, u8 *data)
{
	int ret;
	
	printk(KERN_ALERT "!!!!!!! send %d\n", data[1]);

// 	ret = hid_hw_raw_request(dev->hdev, data[0], data, ALIENFX_BUFFER_SIZE, HID_OUTPUT_REPORT, HID_REQ_SET_REPORT);
// 	if (ret != ALIENFX_BUFFER_SIZE) {
// 		hid_err(led->hdev, "failed to send set report request: %i\n", ret);
// 		if (ret < 0)
// 			return ret;
// 		return -EIO;
// 	}

	return 0;
}

static void alienfx_send_color_cmd(struct alienfx_dev *dev, int region, int block, int color)
{
	CMD_SET_COL[3] = (region >> 16) & 0xff;
	CMD_SET_COL[4] = (region >> 8) & 0xff;
	CMD_SET_COL[5] = region & 0xff;

	CMD_SET_COL[6] = (color >> 4) & 0xff;
	CMD_SET_COL[7] = (color << 4) & 0xf0;

	alienfx_send_cmd(dev, CMD_SET_COL);

}

static void alienfx_color_set(struct led_classdev *led_cdev, enum led_brightness color)
{
	printk(KERN_ALERT "!!!!!!! SETSETSET\n");
	
	struct device *pdev = led_cdev->dev->parent;
	struct hid_device *hdev = container_of(pdev, struct hid_device, dev);
	struct alienfx_dev *dev = hid_get_drvdata(hdev);
	struct alienfx_led *led;

	for (led = dev->leds; led->name; led++) {  //FIXME м.б. сработает container_of?
		if (led_cdev == &led->classdev)
			break;
	}

	printk(KERN_ALERT "!!!!!!! led name = %s\n", led->name);

	if (led->name) {
		led->color = color;
// 		led->state = LED_STATE_NEW_VALUE;
		schedule_work(&dev->work);
	}	
}

static void alienfx_work(struct work_struct *work)
{
	struct alienfx_dev *dev = container_of(work, struct alienfx_dev, work);
	struct alienfx_led *led;
	int block;
	
	mutex_lock(&dev->lock);
	printk(KERN_ALERT "-------------------\n");
	
	alienfx_send_cmd(dev, CMD_RESET);
	msleep(1);
	
	block = 0;
	for (led = dev->leds; led->name; led++) {  //FIXME м.б. сработает container_of?
		if (led->color >= 0) {
			led->color = -1;
			alienfx_send_color_cmd(dev, led->id, block, led->color);
			alienfx_send_cmd(dev, CMD_LOOP);
			block++;
		}
	}
	
	alienfx_send_cmd(dev, CMD_TR_EXEC);
	
	printk(KERN_ALERT "++++++++++++++++++++\n");
	mutex_unlock(&dev->lock);
}

static int alienfx_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	int ret;
	int name_sz;
	char *name;
	const char *devname;
	struct alienfx_dev *dev;
	struct alienfx_led *led;

	dev = devm_kzalloc(&hdev->dev, sizeof(*dev), GFP_KERNEL);
	if (!dev)
		return -ENOMEM;

	mutex_init(&dev->lock);
	
	INIT_WORK(&dev->work, alienfx_work);

	dev->hdev = hdev;
	hid_set_drvdata(hdev, dev);
	

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid parsing failed\n");
		return ret; //FIXME dev освобождается в alienfx_remove или это баг?
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "hw start failed\n");
		return ret; //FIXME dev освобождается в alienfx_remove или это баг?
	}

	dev->leds = NULL; //FIXME
	if (hdev->type == 0) {
		
		dev->leds = (struct alienfx_led *)id->driver_data;
		
		for (led = dev->leds; led->name; led++) {
			devname = dev_name(&hdev->dev);
			name_sz = strlen(devname) + strlen(led->name) + 3;
			
			name = devm_kzalloc(&hdev->dev, name_sz, GFP_KERNEL);
			if (!name) {
				ret = -ENOMEM;
				goto fail;
			}
	
			snprintf(name, name_sz, "%s::%s", devname, led->name);
			led->classdev.name = name;
			led->classdev.max_brightness = 65535;
			led->classdev.brightness_set = alienfx_color_set;
			led->state = LED_STATE_INIT;
			led->color = -1;
			led->dev = dev;
	
			ret = led_classdev_register(&hdev->dev, &led->classdev);
			if (ret) {
				hid_err(hdev, "could not register led device\n");
				goto fail;
			}
		}
	}
	
	return 0;

fail:
	if (dev->leds) { //FIXME
		for (led = dev->leds; led->name; led++) {
			printk(KERN_ALERT "---fail %s 0x%x\n", led->name, led->id);
			led_classdev_unregister(&led->classdev);
		}
	}

	hid_hw_stop(hdev);
	return ret;
}

static void alienfx_remove(struct hid_device *hdev)
{
	struct alienfx_dev *dev = hid_get_drvdata(hdev);
	struct alienfx_led *led;
	
	if (dev->leds) { //FIXME
		for (led = dev->leds; led->name; led++) {
			printk(KERN_ALERT "---unreg %s 0x%x\n", led->name, led->id);
			led_classdev_unregister(&led->classdev);
		}
	}

	hid_hw_stop(hdev);
}

static int alienfx_raw_event(struct hid_device * hdev, struct hid_report *rep, u8 *raw_data, int size)
{
	printk(KERN_ALERT "EVENT\n");
	return 1;
}

static const struct hid_device_id alienfx_id[] = {
	{HID_USB_DEVICE(USB_VENDOR_ID_ALIENFX, USB_DEVICE_ID_ALIENFX_M11X), .driver_data = (kernel_ulong_t) m11x_leds},
	{}
};

static struct hid_driver alienfx_driver = {
	.probe = alienfx_probe,
	.remove = alienfx_remove,
	.name = "alienfx",
	.id_table = alienfx_id,
	.raw_event = alienfx_raw_event
};

module_hid_driver(alienfx_driver);


MODULE_AUTHOR("Alexandr Ivanov");
MODULE_DESCRIPTION("AlienFX driver");
MODULE_LICENSE("GPL");
