// SPDX-License-Identifier: GPL-2.0-only
#include <linux/delay.h>
#include <linux/device.h>
#include <linux/devm-helpers.h>
#include <linux/err.h>
#include <linux/gpio/consumer.h>
#include <linux/interrupt.h>
#include <linux/irq.h>
#include <linux/jiffies.h>
#include <linux/pm_wakeup.h>
#include <linux/property.h>
#include <linux/workqueue.h>

#include <sound/core.h>
#include <sound/jack.h>

#include "ums9117-jack.h"
#include "ums9117-sc2720-codec.h"

/* Stock firmware debounces the jack insert EIC for 200 ms. */
#define UMS9117_JACK_EIC_DEBOUNCE_MS 200U
/* A change is reported once the line has held its new level this long. */
#define UMS9117_JACK_INSERT_CONFIRM_MS 300U
#define UMS9117_JACK_REMOVE_CONFIRM_MS 50U
/* EIC data follows its input 2 ms after the line is requested. */
#define UMS9117_JACK_DATA_DELAY_US 2000U
/*
 * The detector is powered just before the first reading and its settling
 * time is not documented, so the line is read once more after this delay.
 */
#define UMS9117_JACK_SETTLE_MS 300U
/* Keep a woken system up until the longest confirmation has reported. */
#define UMS9117_JACK_WAKE_EVENT_MS 400U

struct ums9117_jack {
	struct device *dev;
	struct gpio_desc *gpiod;
	struct snd_jack *jack;
	/* After create, this work is the only caller of snd_jack_report(). */
	struct delayed_work report_work;
	int irq;
	bool wake_armed;
};

static int report_state(struct ums9117_jack *jack)
{
	int present = gpiod_get_value_cansleep(jack->gpiod);

	if (present < 0)
		return present;
	snd_jack_report(jack->jack, present ? SND_JACK_HEADPHONE : 0);
	return 0;
}

static void ums9117_jack_report_work(struct work_struct *work)
{
	struct ums9117_jack *jack = container_of(
		to_delayed_work(work), struct ums9117_jack, report_work);
	int ret = report_state(jack);

	if (ret)
		dev_err_ratelimited(jack->dev,
				    "cannot read headphone jack: %pe\n",
				    ERR_PTR(ret));
}

static irqreturn_t ums9117_jack_irq_thread(int irq, void *data)
{
	struct ums9117_jack *jack = data;
	int present = gpiod_get_value_cansleep(jack->gpiod);
	unsigned int confirm_ms = present > 0 ? UMS9117_JACK_INSERT_CONFIRM_MS :
						UMS9117_JACK_REMOVE_CONFIRM_MS;

	pm_wakeup_event(jack->dev, UMS9117_JACK_WAKE_EVENT_MS);
	/* Each edge restarts the wait; the work reports read errors. */
	mod_delayed_work(system_wq, &jack->report_work,
			 msecs_to_jiffies(confirm_ms));
	return IRQ_HANDLED;
}

struct ums9117_jack *ums9117_jack_create(struct device *dev,
					 struct snd_card *card,
					 struct ums9117_sc2720_codec *codec)
{
	struct ums9117_jack *jack;
	struct gpio_desc *gpiod;
	int ret;

	gpiod = devm_gpiod_get_optional(dev, "hp-det", GPIOD_IN);
	if (IS_ERR(gpiod))
		return ERR_PTR(
			dev_err_probe(dev, PTR_ERR(gpiod),
				      "cannot get headphone jack line\n"));
	if (!gpiod)
		return NULL;
	jack = devm_kzalloc(dev, sizeof(*jack), GFP_KERNEL);
	if (!jack)
		return ERR_PTR(-ENOMEM);
	jack->dev = dev;
	jack->gpiod = gpiod;

	ret = gpiod_set_debounce(gpiod,
				 UMS9117_JACK_EIC_DEBOUNCE_MS * USEC_PER_MSEC);
	if (ret)
		return ERR_PTR(dev_err_probe(
			dev, ret, "cannot debounce headphone jack\n"));
	ret = ums9117_sc2720_codec_enable_jack_detect(codec);
	if (ret)
		return ERR_PTR(dev_err_probe(
			dev, ret, "cannot power headset detector\n"));
	ret = snd_jack_new(card, "Headphone", SND_JACK_HEADPHONE, &jack->jack,
			   true, false);
	if (ret)
		return ERR_PTR(ret);

	jack->irq = gpiod_to_irq(gpiod);
	if (jack->irq < 0)
		return ERR_PTR(dev_err_probe(
			dev, jack->irq, "cannot get headphone jack IRQ\n"));
	/* Released after the IRQ, so the thread cannot queue it again. */
	ret = devm_delayed_work_autocancel(dev, &jack->report_work,
					   ums9117_jack_report_work);
	if (ret)
		return ERR_PTR(ret);
	ret = devm_request_threaded_irq(
		dev, jack->irq, NULL, ums9117_jack_irq_thread,
		IRQF_ONESHOT | IRQF_NO_AUTOEN | IRQF_TRIGGER_RISING |
			IRQF_TRIGGER_FALLING,
		dev_name(dev), jack);
	if (ret)
		return ERR_PTR(dev_err_probe(
			dev, ret, "cannot request headphone jack IRQ\n"));
	/*
	 * A lazily disabled EIC stays unmasked in the PMIC, whose interrupt
	 * wakes the system whenever an unmasked source fires. disable_irq()
	 * has to mask the line to keep a jack change from waking the system.
	 * free_irq() clears the flag.
	 */
	irq_set_status_flags(jack->irq, IRQ_DISABLE_UNLAZY);
	if (device_property_read_bool(dev, "wakeup-source")) {
		ret = devm_device_init_wakeup(dev);
		if (ret)
			return ERR_PTR(ret);
	}

	usleep_range(UMS9117_JACK_DATA_DELAY_US,
		     2 * UMS9117_JACK_DATA_DELAY_US);
	ret = report_state(jack);
	if (ret)
		return ERR_PTR(dev_err_probe(dev, ret,
					     "cannot read headphone jack\n"));
	return jack;
}

void ums9117_jack_start(struct ums9117_jack *jack)
{
	if (!jack)
		return;
	/* Unmasking first lets a change after the next read raise an event. */
	enable_irq(jack->irq);
	mod_delayed_work(system_wq, &jack->report_work,
			 msecs_to_jiffies(UMS9117_JACK_SETTLE_MS));
}

void ums9117_jack_stop(struct ums9117_jack *jack)
{
	if (!jack)
		return;
	disable_irq(jack->irq);
	cancel_delayed_work_sync(&jack->report_work);
}

int ums9117_jack_suspend(struct ums9117_jack *jack)
{
	int ret;

	if (!jack)
		return 0;
	if (device_may_wakeup(jack->dev)) {
		ret = enable_irq_wake(jack->irq);
		if (ret)
			return ret;
		jack->wake_armed = true;
	} else {
		disable_irq(jack->irq);
	}
	cancel_delayed_work_sync(&jack->report_work);
	return 0;
}

void ums9117_jack_resume(struct ums9117_jack *jack)
{
	int ret;

	if (!jack)
		return;
	if (jack->wake_armed) {
		ret = disable_irq_wake(jack->irq);
		if (ret)
			dev_err(jack->dev,
				"cannot disarm headphone jack wakeup: %pe\n",
				ERR_PTR(ret));
		jack->wake_armed = false;
	} else {
		enable_irq(jack->irq);
	}
	/* A masked line raised no event for a change during sleep. */
	mod_delayed_work(system_wq, &jack->report_work, 0);
}
