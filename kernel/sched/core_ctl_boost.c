// SPDX-License-Identifier: GPL-2.0
/*
 * Unisolate all big cluster CPUs on user interaction and app launches so
 * that the first frames of a gesture or animation don't have to wait for
 * core_ctl to notice the load and bring parked CPUs back.
 */

#define pr_fmt(fmt) "core_ctl_boost: " fmt

#include <linux/fb.h>
#include <linux/init.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/mutex.h>
#include <linux/sched/core_ctl.h>
#include <linux/slab.h>
#include <linux/workqueue.h>

#define INPUT_BOOST_MS	1000
#define LAUNCH_BOOST_MS	1000

static DEFINE_MUTEX(boost_lock);
static bool boost_held;
static bool screen_off;

static void boost_work_fn(struct work_struct *work);
static void unboost_work_fn(struct work_struct *work);
static DECLARE_WORK(boost_work, boost_work_fn);
static DECLARE_DELAYED_WORK(unboost_work, unboost_work_fn);

static void boost_work_fn(struct work_struct *work)
{
	mutex_lock(&boost_lock);
	if (!boost_held && !READ_ONCE(screen_off)) {
		boost_held = true;
		core_ctl_set_boost(true);
	}
	mutex_unlock(&boost_lock);
}

static void unboost_work_fn(struct work_struct *work)
{
	mutex_lock(&boost_lock);
	if (boost_held) {
		boost_held = false;
		core_ctl_set_boost(false);
	}
	mutex_unlock(&boost_lock);
}

void core_ctl_boost_kick(unsigned int duration_ms)
{
	if (READ_ONCE(screen_off))
		return;

	/* Extend the boost; only queue the boost work if it isn't held yet */
	mod_delayed_work(system_unbound_wq, &unboost_work,
			 msecs_to_jiffies(duration_ms));
	if (!READ_ONCE(boost_held))
		queue_work(system_highpri_wq, &boost_work);
}

static void core_ctl_boost_input_event(struct input_handle *handle,
				       unsigned int type, unsigned int code,
				       int value)
{
	core_ctl_boost_kick(INPUT_BOOST_MS);
}

static int core_ctl_boost_input_connect(struct input_handler *handler,
					struct input_dev *dev,
					const struct input_device_id *id)
{
	struct input_handle *handle;
	int ret;

	handle = kzalloc(sizeof(*handle), GFP_KERNEL);
	if (!handle)
		return -ENOMEM;

	handle->dev = dev;
	handle->handler = handler;
	handle->name = "core_ctl_boost";

	ret = input_register_handle(handle);
	if (ret)
		goto free_handle;

	ret = input_open_device(handle);
	if (ret)
		goto unregister_handle;

	return 0;

unregister_handle:
	input_unregister_handle(handle);
free_handle:
	kfree(handle);
	return ret;
}

static void core_ctl_boost_input_disconnect(struct input_handle *handle)
{
	input_close_device(handle);
	input_unregister_handle(handle);
	kfree(handle);
}

static const struct input_device_id core_ctl_boost_ids[] = {
	/* Multi-touch touchscreen */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT |
			INPUT_DEVICE_ID_MATCH_ABSBIT,
		.evbit = { BIT_MASK(EV_ABS) },
		.absbit = { [BIT_WORD(ABS_MT_POSITION_X)] =
			BIT_MASK(ABS_MT_POSITION_X) |
			BIT_MASK(ABS_MT_POSITION_Y) }
	},
	/* Keypad */
	{
		.flags = INPUT_DEVICE_ID_MATCH_EVBIT,
		.evbit = { BIT_MASK(EV_KEY) }
	},
	{ }
};

static struct input_handler core_ctl_boost_input_handler = {
	.event		= core_ctl_boost_input_event,
	.connect	= core_ctl_boost_input_connect,
	.disconnect	= core_ctl_boost_input_disconnect,
	.name		= "core_ctl_boost",
	.id_table	= core_ctl_boost_ids
};

static int fb_notifier_cb(struct notifier_block *nb, unsigned long action,
			  void *data)
{
	int *blank = ((struct fb_event *)data)->data;

	if (action != FB_EARLY_EVENT_BLANK)
		return NOTIFY_OK;

	if (*blank == FB_BLANK_UNBLANK) {
		WRITE_ONCE(screen_off, false);
	} else {
		WRITE_ONCE(screen_off, true);
		mod_delayed_work(system_unbound_wq, &unboost_work, 0);
	}

	return NOTIFY_OK;
}

static struct notifier_block fb_notif = {
	.notifier_call = fb_notifier_cb,
	.priority = INT_MAX
};

static int __init core_ctl_boost_init(void)
{
	int ret;

	ret = input_register_handler(&core_ctl_boost_input_handler);
	if (ret) {
		pr_err("Failed to register input handler, err: %d\n", ret);
		return ret;
	}

	ret = fb_register_client(&fb_notif);
	if (ret) {
		pr_err("Failed to register fb notifier, err: %d\n", ret);
		input_unregister_handler(&core_ctl_boost_input_handler);
	}

	return ret;
}
late_initcall(core_ctl_boost_init);
