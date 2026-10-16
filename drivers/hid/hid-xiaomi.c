// SPDX-License-Identifier: GPL-2.0-or-later
/*
 *  HID driver for the Xiaomi Mi Pad 5 keyboard.
 *
 *  Copyright (c) 2024 Sebastiano Barezzi <seba@sebaubuntu.dev>
 */

/**
 * This driver is needed to support the Xiaomi Mi Pad 5 keyboard due to their engineering mess.
 * The keyboard controller is in the tablet, and the keyboard connects through pogo pins.
 * The controllers appears as a USB HID device, but without being aware of its presence.
 * The driver, thanks to the original Xiaomi driver needed for power juice, will add and remove the
 * Linux input device when the keyboard is connected and disconnected.
 */

#include <linux/hid.h>
#include <linux/input.h>
#include <linux/input/usbkbd-xiaomi.h>
#include <linux/list.h>
#include <linux/module.h>
#include <linux/completion.h>
#include <linux/mutex.h>
#include <linux/slab.h>
#include <linux/spinlock.h>
#include <linux/usb.h>
#include <linux/types.h>
#include <linux/workqueue.h>

#include "hid-ids.h"

/**
 * Wait interval for the connection check to let the IRQ handler to settle down.
 */
#define CONNECTION_WAIT_INTERVAL 100 /* msec */

/**
 * Connection status query, sent through the vendor interface of the keyboard controller.
 */
#define CONNECTION_QUERY_REPORT_ID 0x4e
#define CONNECTION_QUERY_TIMEOUT 500 /* msec */
#define CONNECTION_RESPONSE_MIN_SIZE 19
#define CONNECTION_RESPONSE_MAGIC 0xa2

static const u8 xiaomi_connection_command[] = {
	CONNECTION_QUERY_REPORT_ID, 0x31, 0x80, 0x38, 0xa1, 0x01, 0x01, 0xda,
};

struct xiaomi_priv {
	struct list_head list;

	struct hid_device *hdev;

	bool enabled;
	struct mutex enabled_lock;

	struct workqueue_struct *connection_wq;
	struct delayed_work connection_work;

	// Vendor interface, used for connection queries and never stopped
	bool query_capable;
	spinlock_t query_lock;
	bool query_pending;
	bool query_connected;
	struct completion query_done;
};

// List of all instances
static struct list_head instances = LIST_HEAD_INIT(instances);
static DEFINE_MUTEX(instances_lock);

// Keyboard connection status, true by default
static bool connected = true;

static bool xiaomi_connection_response_connected(const u8 *response)
{
	if (response[18] == 1)
		return false;

	if ((response[9] & 0x03) == 0)
		return false;

	return (response[9] & 0x63) == 0x23;
}

static int xiaomi_raw_event(struct hid_device *hdev, struct hid_report *report,
			    u8 *data, int size)
{
	struct xiaomi_priv *priv = hid_get_drvdata(hdev);
	unsigned long flags;

	if (!priv->query_capable || size < CONNECTION_RESPONSE_MIN_SIZE ||
	    data[4] != CONNECTION_RESPONSE_MAGIC)
		return 0;

	spin_lock_irqsave(&priv->query_lock, flags);
	if (priv->query_pending) {
		priv->query_connected =
			xiaomi_connection_response_connected(data);
		priv->query_pending = false;
		complete(&priv->query_done);
	}
	spin_unlock_irqrestore(&priv->query_lock, flags);

	return 0;
}

static int xiaomi_query(struct xiaomi_priv *priv, bool *state)
{
	struct hid_device *hdev = priv->hdev;
	unsigned long flags;
	u8 *buf;
	int ret;

	// Must be DMA-able
	buf = kmemdup(xiaomi_connection_command,
		      sizeof(xiaomi_connection_command), GFP_KERNEL);
	if (!buf)
		return -ENOMEM;

	// Make sure the input endpoint is being polled
	ret = hid_hw_open(hdev);
	if (ret)
		goto out_free;

	spin_lock_irqsave(&priv->query_lock, flags);
	reinit_completion(&priv->query_done);
	priv->query_pending = true;
	spin_unlock_irqrestore(&priv->query_lock, flags);

	ret = hid_hw_output_report(hdev, buf,
				   sizeof(xiaomi_connection_command));
	if (ret < 0) {
		hid_err(hdev, "connection query failed: %d\n", ret);
		goto out_close;
	}

	if (!wait_for_completion_timeout(
		    &priv->query_done,
		    msecs_to_jiffies(CONNECTION_QUERY_TIMEOUT))) {
		ret = -ETIMEDOUT;
		goto out_close;
	}

	*state = priv->query_connected;
	ret = 0;

out_close:
	spin_lock_irqsave(&priv->query_lock, flags);
	priv->query_pending = false;
	spin_unlock_irqrestore(&priv->query_lock, flags);

	hid_hw_close(hdev);
out_free:
	kfree(buf);

	return ret;
}

// Called by vendor driver
int xiaomi_keyboard_connection_query(bool *state)
{
	struct xiaomi_priv *instance;
	int ret = -ENODEV;

	// Held for the whole query to keep the instance from being removed
	mutex_lock(&instances_lock);
	list_for_each_entry(instance, &instances, list) {
		if (instance->query_capable) {
			ret = xiaomi_query(instance, state);
			break;
		}
	}
	mutex_unlock(&instances_lock);

	return ret;
}

// Called by vendor driver
void xiaomi_keyboard_connection_change(bool _connected)
{
	struct xiaomi_priv *instance;

	connected = _connected;

	mutex_lock(&instances_lock);
	list_for_each_entry(instance, &instances, list) {
		// Schedule the work, wait a sec before toggling the keyboard
		mod_delayed_work(instance->connection_wq,
				 &instance->connection_work,
				 msecs_to_jiffies(CONNECTION_WAIT_INTERVAL));
	}
	mutex_unlock(&instances_lock);
}

/**
 * Updates the HID device status in a safe way.
 * Needed to avoid a double-free in the HID core.
 */
static int xiaomi_safe_toggle(struct hid_device *hdev, bool enable)
{
	struct xiaomi_priv *priv = hid_get_drvdata(hdev);
	int ret = 0;

	mutex_lock(&priv->enabled_lock);

	if (priv->enabled == enable) {
		goto done;
	}

	if (enable) {
		ret = hid_hw_start(hdev, HID_CONNECT_DEFAULT);
		if (ret) {
			hid_err(hdev, "hid_hw_start failed\n");
			goto done;
		}
	} else {
		hid_hw_stop(hdev);
	}

	priv->enabled = enable;

done:
	mutex_unlock(&priv->enabled_lock);

	return ret;
}

static void xiaomi_connection_work(struct work_struct *work)
{
	struct xiaomi_priv *priv = container_of(
		to_delayed_work(work), struct xiaomi_priv, connection_work);
	struct hid_device *hdev = priv->hdev;

	// Keep the vendor interface alive to be able to query the connection status
	if (priv->query_capable)
		return;

	xiaomi_safe_toggle(hdev, connected);
}

static int xiaomi_input_configured(struct hid_device *hdev,
				   struct hid_input *hi)
{
	struct input_dev *input = hi->input;

	// Drop EV_REL, the keyboard doesn't have a touchpad
	__clear_bit(EV_REL, input->evbit);

	return 0;
}

static int xiaomi_probe(struct hid_device *hdev, const struct hid_device_id *id)
{
	struct xiaomi_priv *priv;
	int ret;

	if (!hid_is_usb(hdev)) {
		ret = -EINVAL;
		goto err_1;
	}

	priv = kzalloc(sizeof(*priv), GFP_KERNEL);
	if (!priv) {
		ret = -ENOMEM;
		goto err_1;
	}

	// Setup workqueue
	priv->connection_wq = create_singlethread_workqueue("hid-xiaomi");
	if (!priv->connection_wq) {
		ret = -ENOMEM;
		goto err_2;
	}
	INIT_DELAYED_WORK(&priv->connection_work, xiaomi_connection_work);

	priv->hdev = hdev;
	priv->enabled = false;

	// Setup the mutex
	mutex_init(&priv->enabled_lock);

	spin_lock_init(&priv->query_lock);
	init_completion(&priv->query_done);

	hid_set_drvdata(hdev, priv);

	ret = hid_parse(hdev);
	if (ret) {
		hid_err(hdev, "hid_parse failed\n");
		goto err_3;
	}

	priv->query_capable =
		!!hdev->report_enum[HID_OUTPUT_REPORT]
			  .report_id_hash[CONNECTION_QUERY_REPORT_ID];

	ret = xiaomi_safe_toggle(hdev, true);
	if (ret) {
		hid_err(hdev, "hid_hw_start failed\n");
		goto err_3;
	}

	// Schedule an update to keep the keyboard in sync
	mod_delayed_work(priv->connection_wq, &priv->connection_work,
			 msecs_to_jiffies(CONNECTION_WAIT_INTERVAL));

	// Add the instance to the list
	mutex_lock(&instances_lock);
	list_add(&priv->list, &instances);
	mutex_unlock(&instances_lock);

	// Let the vendor driver know the connection status can be queried now
	if (priv->query_capable)
		xiaomi_keyboard_request_query();

	return 0;

err_3:
	mutex_destroy(&priv->enabled_lock);
	destroy_workqueue(priv->connection_wq);
err_2:
	kfree(priv);
err_1:
	return ret;
}

static void xiaomi_remove(struct hid_device *hdev)
{
	struct xiaomi_priv *priv = hid_get_drvdata(hdev);

	// Remove the instance from the list
	mutex_lock(&instances_lock);
	list_del(&priv->list);
	mutex_unlock(&instances_lock);

	// Wait for the work to finish
	cancel_delayed_work_sync(&priv->connection_work);

	// Stop the HID device
	xiaomi_safe_toggle(hdev, false);

	// Destroy the mutex
	mutex_destroy(&priv->enabled_lock);

	// Destroy the workqueue
	destroy_workqueue(priv->connection_wq);

	kfree(priv);
}

static const struct hid_device_id xiaomi_devices[] = {
	{
		HID_USB_DEVICE(USB_VENDOR_ID_NANO_IC, 0x3FFC),
	},
	{}
};
MODULE_DEVICE_TABLE(hid, xiaomi_devices);

static struct hid_driver hid_xiaomi = {
	.name = "hid-xiaomi",
	.id_table = xiaomi_devices,
	.probe = xiaomi_probe,
	.remove = xiaomi_remove,
	.input_configured = xiaomi_input_configured,
	.raw_event = xiaomi_raw_event,
};
module_hid_driver(hid_xiaomi);

MODULE_AUTHOR("Sebastiano Barezzi <seba@sebaubuntu.dev>");
MODULE_DESCRIPTION("Xiaomi Mi Pad 5 keyboard HID driver");
MODULE_LICENSE("GPL");
