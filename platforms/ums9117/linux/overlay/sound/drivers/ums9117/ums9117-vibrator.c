// SPDX-License-Identifier: GPL-2.0-only
#include <linux/err.h>
#include <linux/input.h>
#include <linux/jiffies.h>
#include <linux/workqueue.h>

#include "ums9117-pcm-internal.h"

#define UMS9117_VIBRATOR_NAME "UMS9117 speaker vibrator"
#define UMS9117_VIBRATOR_PHYS "fplinux/vibrator0"
#define UMS9117_VIBRATOR_MAX_ON_MS 5000U

static void ums9117_vibrator_play_work(struct work_struct *work)
{
	struct ums9117_vibrator *vibrator =
		container_of(work, struct ums9117_vibrator, play_work);
	struct ums9117_pcm *audio =
		container_of(vibrator, struct ums9117_pcm, vibrator);
	unsigned long flags;
	bool on;
	int ret = 0;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	on = vibrator->requested && !vibrator->suspended && !vibrator->stopping;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);

	mutex_lock(&audio->lock);
	if (on)
		ret = ums9117_pcm_vibrate_locked(audio);
	if (!on || ret)
		ums9117_pcm_vibrate_off_locked(audio);
	mutex_unlock(&audio->lock);
	if (ret)
		dev_err_ratelimited(audio->dev, "cannot start vibration: %pe\n",
				    ERR_PTR(ret));
	if (on && !ret)
		return;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (ret)
		vibrator->requested = false;
	vibrator->cutoff_latched = false;
	vibrator->off_pending = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	cancel_delayed_work(&vibrator->stop_work);
}

/* Ends an activation at its deadline; a new one needs the confirmed stop. */
static void ums9117_vibrator_stop_work(struct work_struct *work)
{
	struct ums9117_vibrator *vibrator = container_of(
		to_delayed_work(work), struct ums9117_vibrator, stop_work);
	unsigned long delay = 0;
	unsigned long flags;
	bool stop = false;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (vibrator->off_pending) {
		stop = true;
	} else if (vibrator->requested) {
		if (time_before(jiffies, vibrator->stop_deadline)) {
			delay = vibrator->stop_deadline - jiffies;
		} else {
			vibrator->requested = false;
			vibrator->cutoff_latched = true;
			vibrator->off_pending = true;
			stop = true;
		}
	}
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	if (delay)
		mod_delayed_work(system_percpu_wq, &vibrator->stop_work, delay);
	else if (stop)
		schedule_work(&vibrator->play_work);
}

static void ums9117_vibrator_hold_work(struct work_struct *work)
{
	struct ums9117_vibrator *vibrator = container_of(
		to_delayed_work(work), struct ums9117_vibrator, hold_work);
	struct ums9117_pcm *audio =
		container_of(vibrator, struct ums9117_pcm, vibrator);

	mutex_lock(&audio->lock);
	if (!vibrator->tone_on)
		ums9117_pcm_end_vibration_locked(audio);
	mutex_unlock(&audio->lock);
}

/* Called atomically by ff-memless; hardware changes run in play_work. */
static int ums9117_vibrator_play(struct input_dev *input, void *data,
				 struct ff_effect *effect)
{
	struct ums9117_pcm *audio = input_get_drvdata(input);
	struct ums9117_vibrator *vibrator = &audio->vibrator;
	bool on = effect->u.rumble.strong_magnitude ||
		  effect->u.rumble.weak_magnitude;
	unsigned long delay = 0;
	unsigned long flags;
	bool force_stop = false;
	int ret = 0;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	if (vibrator->suspended || vibrator->stopping) {
		ret = -ESHUTDOWN;
	} else if (on && (vibrator->cutoff_latched || vibrator->off_pending)) {
		ret = -EBUSY;
	} else {
		if (on) {
			if (!vibrator->requested)
				vibrator->stop_deadline =
					jiffies +
					msecs_to_jiffies(
						UMS9117_VIBRATOR_MAX_ON_MS);
			if (time_before(jiffies, vibrator->stop_deadline)) {
				delay = vibrator->stop_deadline - jiffies;
			} else {
				on = false;
				vibrator->cutoff_latched = true;
				force_stop = true;
				ret = -EBUSY;
			}
		}
		vibrator->requested = on;
		if (!on)
			vibrator->off_pending = true;
	}
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	if (ret && !force_stop)
		return ret;
	if (on)
		mod_delayed_work(system_percpu_wq, &vibrator->stop_work, delay);
	schedule_work(&vibrator->play_work);
	return ret;
}

/* Stops at once without the hold; the caller has blocked new activations. */
void ums9117_vibrator_stop(struct ums9117_pcm *audio)
{
	struct ums9117_vibrator *vibrator = &audio->vibrator;
	unsigned long flags;

	spin_lock_irqsave(&vibrator->state_lock, flags);
	vibrator->requested = false;
	vibrator->off_pending = true;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
	cancel_delayed_work_sync(&vibrator->stop_work);
	cancel_work_sync(&vibrator->play_work);
	cancel_delayed_work_sync(&vibrator->hold_work);
	mutex_lock(&audio->lock);
	ums9117_pcm_end_vibration_locked(audio);
	mutex_unlock(&audio->lock);
	spin_lock_irqsave(&vibrator->state_lock, flags);
	vibrator->cutoff_latched = false;
	vibrator->off_pending = false;
	spin_unlock_irqrestore(&vibrator->state_lock, flags);
}

static void ums9117_vibrator_close(struct input_dev *input)
{
	ums9117_vibrator_stop(input_get_drvdata(input));
}

void ums9117_vibrator_set_lifecycle(struct ums9117_pcm *audio, bool *flag,
				    bool value)
{
	unsigned long flags;

	spin_lock_irqsave(&audio->vibrator.state_lock, flags);
	*flag = value;
	spin_unlock_irqrestore(&audio->vibrator.state_lock, flags);
}

int ums9117_vibrator_register(struct ums9117_pcm *audio)
{
	struct input_dev *input;
	int ret;

	input = devm_input_allocate_device(audio->dev);
	if (!input)
		return -ENOMEM;
	input->name = UMS9117_VIBRATOR_NAME;
	input->phys = UMS9117_VIBRATOR_PHYS;
	input->id.bustype = BUS_HOST;
	input->close = ums9117_vibrator_close;
	input_set_drvdata(input, audio);
	input_set_capability(input, EV_FF, FF_RUMBLE);
	/* ff-memless frees its data pointer, so pass none. */
	ret = input_ff_create_memless(input, NULL, ums9117_vibrator_play);
	if (ret)
		return ret;
	return input_register_device(input);
}

void ums9117_vibrator_init(struct ums9117_pcm *audio)
{
	spin_lock_init(&audio->vibrator.state_lock);
	INIT_WORK(&audio->vibrator.play_work, ums9117_vibrator_play_work);
	INIT_DELAYED_WORK(&audio->vibrator.stop_work,
			  ums9117_vibrator_stop_work);
	INIT_DELAYED_WORK(&audio->vibrator.hold_work,
			  ums9117_vibrator_hold_work);
}
