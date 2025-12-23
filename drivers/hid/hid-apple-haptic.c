// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *   Apple MTP Haptic Actuator Driver
 */

#include <linux/device.h>
#include <linux/hid.h>
#include <linux/input.h>
#include <linux/module.h>
#include <linux/workqueue.h>

#include "hid-ids.h"

struct apple_haptic {
	struct hid_device *hdev;
	struct input_dev *input;
	bool enabled; // has click handoff ben sent
};

static int apple_haptic_send(struct hid_device *hdev, u8 *data, int size)
{
	int ret;
	u8* buf;

	buf = kmemdup(data, size, GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	/* To change strength or enable/disable, needs value of 2, 0.
	 * So that dockchannel-hid iface->out_flags = 0x80 */
	ret = hid_hw_raw_request(hdev, buf[0], buf, size,
			buf[0] == 0x53 ? HID_OUTPUT_REPORT
			: HID_FEATURE_REPORT, HID_REQ_SET_REPORT);

	kfree(buf);

	if (ret != size)
		return -1; // not sure

	return 0;
}


static int apple_haptic_handoff(struct hid_device *hdev, u8 value)
{
	u8 feature[] = { 0x21, value };
	return apple_haptic_send(hdev, feature, sizeof(feature));
}

static ssize_t raw_store(struct device *dev, struct device_attribute *attr,
		const char *buf, size_t count)
{
	u8 data[19];
	char hex_buf[2];
	struct apple_haptic *haptic = dev_get_drvdata(dev);
	size_t len = count / 2;

	if (len != 19 && len != 9 && len != 4 && len != 2)
		return -EINVAL;

	for (size_t i = 0; i < len; i++) {
		hex_buf[0] = buf[i * 2];
		hex_buf[1] = buf[i * 2 + 1];
		if (kstrtou8(hex_buf, 16, &data[i]))
			return -EINVAL;

	}
	if (!apple_haptic_send(haptic->hdev, data, len) && data[0] == 0x21)
		haptic->enabled = data[1];
;
	return count;
}

// TODO: maybe not the best permissions
static DEVICE_ATTR(raw, 0220, NULL, raw_store);

static struct attribute *haptic_attrs[] = {
	&dev_attr_raw.attr,
	NULL,
};

static const struct attribute_group haptic_attr_group = {
	.attrs = haptic_attrs,
};

static int apple_haptic_probe(struct hid_device *hdev,
		const struct hid_device_id *id)
{
	struct apple_haptic *haptic;
	struct input_dev *input;
	int ret;

	if (strstr(hdev->name, "actuator") == NULL)
		return -ENODEV;

	haptic = devm_kzalloc(&hdev->dev, sizeof(*haptic), GFP_KERNEL);
	if (!haptic) {
		hid_err(hdev, "can't alloc apple_haptic descriptor\n");
		return -ENOMEM;
	}
	haptic->enabled = false;

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "apple_haptic hid parse failed\n");
		return ret;
	}

	ret = hid_hw_start(hdev, HID_CONNECT_HIDRAW);
	if (ret) {
		hid_err(hdev, "apple_haptic hw start failed\n");
		return ret;
	}

	haptic->hdev = hdev;
	hid_set_drvdata(hdev, haptic);

	input = devm_input_allocate_device(&hdev->dev);
	if (!input) {
		ret = -ENOMEM;
		goto err_stop;
	}

	input->name = hdev->name;
	input->id.bustype = hdev->bus;
	input->id.vendor = hdev->vendor;
	input->id.product = hdev->product;
	input->id.version = hdev->version;
	input->dev.parent = &hdev->dev;

	input_set_drvdata(input, haptic);

	ret = input_register_device(input);
	if (ret) {
		hid_err(hdev, "apple_haptic failed to register input device\n");
		goto err_stop;
	}

	haptic->input = input;

	ret = sysfs_create_group(&haptic->input->dev.kobj, &haptic_attr_group);
	if (ret) {
		hid_err(hdev, "Failed to create sysfs attributes\n");
	}

	return 0;

err_stop:
	hid_hw_stop(hdev);
	return ret;
}

static void apple_haptic_remove(struct hid_device *hdev)
{
	struct apple_haptic *haptic = hid_get_drvdata(hdev);

	if (haptic) {
		sysfs_remove_group(&haptic->input->dev.kobj, &haptic_attr_group);
		if (haptic->enabled)
			apple_haptic_handoff(hdev, 0x00);

	}
	hid_hw_stop(hdev);
}


static const struct hid_device_id apple_haptic_devices[] = {
	{ HID_DEVICE(BUS_HOST, HID_GROUP_ANY, HOST_VENDOR_ID_APPLE,
			HID_ANY_ID), .driver_data = 0 },
	{ }
};

MODULE_DEVICE_TABLE(hid, apple_haptic_devices);

static struct hid_driver apple_haptic_driver = {
	.name = "apple-haptic",
	.id_table = apple_haptic_devices,
	.probe = apple_haptic_probe,
	.remove = apple_haptic_remove,
};
module_hid_driver(apple_haptic_driver);

MODULE_DESCRIPTION("Apple MTP Haptic Actuator Driver");
MODULE_LICENSE("GPL");
