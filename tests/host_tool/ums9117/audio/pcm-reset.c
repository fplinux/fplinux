// SPDX-License-Identifier: GPL-2.0-only
#include <stdarg.h>

#include "pcm-reset-alsa.h"
#include "ums9117-audio-profile.h"
#include "ums9117-jack.h"
#include "ums9117-sc2720-codec.h"

extern struct platform_driver *pcm_reset_driver;

/* FIFO samples and occupancies are controlled; no analog audio is modeled. */
struct ums9117_audio {
	unsigned int queued;
	unsigned int capture_available;
	unsigned int captured_samples;
	unsigned int playback_pairs;
	unsigned int capture_errors;
	unsigned int playback_starts;
	unsigned int capture_starts;
	bool playback_running;
	bool capture_running;
};
struct ums9117_sc2720_codec {
	bool playback_prepared;
	bool capture_prepared;
};

static struct ums9117_audio digital;
static struct ums9117_sc2720_codec codec;
static struct snd_card card;
static struct snd_pcm pcm;
static struct task_struct notification_task;
static struct hrtimer *service_timer;
static struct snd_pcm_substream streams[2];
static struct snd_pcm_runtime runtimes[2];
static struct snd_pcm_mmap_status statuses[2];
static struct snd_pcm_mmap_control controls[2];
static __le16 playback_samples[1920 * 2];
static __le16 capture_samples[1920];
static void *device_allocation;
static unsigned int driver_errors;
static unsigned int failures;
static bool interleave_reset;
static bool capture_period_delivered;
unsigned long jiffies;

#define CHECK(condition)                                                   \
	do {                                                               \
		if (!(condition)) {                                        \
			fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, \
				#condition);                               \
			failures++;                                        \
		}                                                          \
	} while (0)

void snd_pcm_stream_lock(struct snd_pcm_substream *substream)
{
	assert(substream->pcm->nonatomic);
	mutex_lock(&substream->stream_mutex);
}

void snd_pcm_stream_unlock(struct snd_pcm_substream *substream)
{
	mutex_unlock(&substream->stream_mutex);
}

/* The generic ALSA RESET folds a running hardware pointer into the ring. */
int snd_pcm_lib_ioctl(struct snd_pcm_substream *substream, unsigned int command,
		      void *argument)
{
	struct snd_pcm_runtime *runtime = substream->runtime;

	if (command != SNDRV_PCM_IOCTL1_RESET)
		return -ENXIO;
	snd_pcm_stream_lock(substream);
	if (snd_pcm_running(substream)) {
		runtime->status->hw_ptr =
			pcm.ops[substream->stream]->pointer(substream);
		runtime->hw_ptr_jiffies = jiffies;
	} else {
		runtime->status->hw_ptr = 0;
		runtime->hw_ptr_wrap = 0;
	}
	snd_pcm_stream_unlock(substream);
	return 0;
}

static void run_timer_batch(void)
{
	assert(service_timer->active && digital.playback_running);
	assert(digital.capture_running && digital.capture_available == 0);
	/* At 48 kHz a five millisecond step consumes 240 of 320 FIFO frames. */
	assert(digital.queued == 320);
	digital.queued = 80;
	digital.capture_available = 240;
	jiffies += 5;
	assert(service_timer->function(service_timer) == HRTIMER_RESTART);
	CHECK(digital.queued == 320);
	if (runtimes[SNDRV_PCM_STREAM_PLAYBACK].state ==
	    SNDRV_PCM_STATE_RUNNING)
		controls[SNDRV_PCM_STREAM_PLAYBACK].appl_ptr += 240;
}

/* Model only the unlinked ALSA RESET before/after-driver bookkeeping. */
static int reset_stream(struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	int ret;

	assert(!substream->stream_mutex.locked);
	assert(runtime->state == SNDRV_PCM_STATE_PREPARED ||
	       runtime->state == SNDRV_PCM_STATE_RUNNING);
	mutex_lock(&runtime->buffer_mutex);
	ret = pcm.ops[substream->stream]->ioctl(substream,
						SNDRV_PCM_IOCTL1_RESET, NULL);
	assert(!substream->stream_mutex.locked);
	if (ret)
		goto out;
	if (interleave_reset && substream->stream == SNDRV_PCM_STREAM_CAPTURE)
		run_timer_batch();
	snd_pcm_stream_lock(substream);
	runtime->hw_ptr_base = 0;
	runtime->hw_ptr_interrupt =
		runtime->status->hw_ptr -
		runtime->status->hw_ptr % runtime->period_size;
	runtime->silence_start = runtime->status->hw_ptr;
	runtime->silence_filled = 0;
	snd_pcm_stream_unlock(substream);
	snd_pcm_stream_lock(substream);
	runtime->control->appl_ptr = runtime->status->hw_ptr;
	snd_pcm_stream_unlock(substream);
out:
	mutex_unlock(&runtime->buffer_mutex);
	return ret;
}

/* A client reads every published sample; notification timing is controlled. */
static void publish_capture(unsigned int frames)
{
	struct snd_pcm_runtime *runtime = &runtimes[SNDRV_PCM_STREAM_CAPTURE];
	snd_pcm_uframes_t position;

	position = pcm.ops[SNDRV_PCM_STREAM_CAPTURE]->pointer(
		&streams[SNDRV_PCM_STREAM_CAPTURE]);
	runtime->status->hw_ptr += frames;
	CHECK(position == runtime->status->hw_ptr % 1920);
	runtime->hw_ptr_base = runtime->status->hw_ptr - position;
	runtime->control->appl_ptr = runtime->status->hw_ptr;
}

static void check_capture_sample(unsigned int expected)
{
	snd_pcm_uframes_t position = pcm.ops[SNDRV_PCM_STREAM_CAPTURE]->pointer(
		&streams[SNDRV_PCM_STREAM_CAPTURE]);
	snd_pcm_uframes_t last = position ? position - 1 : 1919;

	CHECK(digital.capture_errors == 0);
	CHECK(digital.capture_available == 0);
	CHECK(digital.captured_samples == expected);
	CHECK(capture_samples[last] == expected);
}

void *devm_kzalloc(struct device *dev, size_t size, unsigned int flags)
{
	assert(!device_allocation);
	device_allocation = calloc(1, size);
	return device_allocation;
}

void *dev_get_drvdata(struct device *dev)
{
	return dev->driver_data;
}

void platform_set_drvdata(struct platform_device *pdev, void *data)
{
	pdev->dev.driver_data = data;
}

void *platform_get_drvdata(struct platform_device *pdev)
{
	return pdev->dev.driver_data;
}

const char *dev_driver_string(struct device *dev)
{
	return "host-pcm";
}

int dev_err_probe(struct device *dev, int error, const char *format, ...)
{
	return error;
}

void pcm_reset_log(const char *format, ...)
{
	va_list arguments;

	driver_errors++;
	va_start(arguments, format);
	vfprintf(stderr, format, arguments);
	va_end(arguments);
}

bool device_property_read_bool(struct device *dev, const char *name)
{
	return !strcmp(name, "fplinux,microphone-capture");
}

bool device_property_present(struct device *dev, const char *name)
{
	return false;
}

int device_property_read_string(struct device *dev, const char *name,
				const char **value)
{
	assert(!strcmp(name, "fplinux,card-longname"));
	*value = "Host PCM callback test";
	return 0;
}

void hrtimer_setup(struct hrtimer *timer,
		   enum hrtimer_restart (*function)(struct hrtimer *timer),
		   int clock, int mode)
{
	assert(!service_timer);
	timer->function = function;
	service_timer = timer;
}

void hrtimer_start(struct hrtimer *timer, ktime_t delay, int mode)
{
	timer->active = true;
}

int hrtimer_cancel(struct hrtimer *timer)
{
	timer->active = false;
	return 0;
}

void hrtimer_forward_now(struct hrtimer *timer, ktime_t delay)
{
	assert(timer == service_timer && delay == 1000000);
}

/* Timer and notification callbacks run only when the scenario schedules them. */
struct task_struct *kthread_run(int (*function)(void *data), void *data,
				const char *name)
{
	notification_task.function = function;
	notification_task.data = data;
	return &notification_task;
}

bool kthread_should_stop(void)
{
	return notification_task.stop_requested;
}

void pcm_reset_wait(wait_queue_head_t *queue, bool ready)
{
	assert(queue);
	if (ready)
		return;
	/* Model an owner's stop request waking an idle notification consumer. */
	assert(notification_task.stop_when_idle);
	notification_task.stop_requested = true;
}

int kthread_stop(struct task_struct *task)
{
	task->stop_requested = true;
	return 0;
}

void sched_set_fifo_low(struct task_struct *task)
{
}

int snd_devm_card_new(struct device *dev, int index, const char *id,
		      void *module, size_t extra, struct snd_card **result)
{
	*result = &card;
	return 0;
}

int snd_card_free_on_error(struct device *dev, int error)
{
	return error;
}

int snd_card_register(struct snd_card *registered)
{
	assert(registered == &card);
	return 0;
}

void snd_card_disconnect(struct snd_card *registered)
{
}

int snd_pcm_new(struct snd_card *registered, const char *name, int index,
		unsigned int playback, unsigned int capture,
		struct snd_pcm **result)
{
	assert(registered == &card && playback == 1 && capture == 1);
	*result = &pcm;
	return 0;
}

void snd_pcm_set_ops(struct snd_pcm *registered, int stream,
		     const struct snd_pcm_ops *ops)
{
	assert(registered == &pcm && stream >= 0 && stream < 2);
	pcm.ops[stream] = ops;
}

int snd_pcm_set_managed_buffer_all(struct snd_pcm *registered, int type,
				   void *data, size_t initial, size_t maximum)
{
	return 0;
}

int snd_pcm_hw_constraint_step(struct snd_pcm_runtime *runtime,
			       unsigned int flags, unsigned int parameter,
			       unsigned long step)
{
	assert(step == 480);
	return 0;
}

int snd_pcm_stop(struct snd_pcm_substream *substream, snd_pcm_state_t state)
{
	int ret;

	assert(substream->stream_mutex.locked);
	ret = pcm.ops[substream->stream]->trigger(substream,
						  SNDRV_PCM_TRIGGER_STOP);
	if (!ret)
		substream->runtime->state = state;
	return ret;
}

/*
 * This ALSA model covers capture pointer updates with period notifications and
 * no boundary crossing, debug corrections, audio timestamps or no-period-wakeup
 * mode.
 */
void snd_pcm_period_elapsed_under_stream_lock(
	struct snd_pcm_substream *substream)
{
	struct snd_pcm_runtime *runtime = substream->runtime;
	snd_pcm_uframes_t position;
	snd_pcm_uframes_t base;
	snd_pcm_uframes_t updated;
	snd_pcm_uframes_t delta;
	snd_pcm_uframes_t available;

	assert(substream->stream == SNDRV_PCM_STREAM_CAPTURE);
	assert(substream->stream_mutex.locked);
	if (!snd_pcm_running(substream))
		return;
	capture_period_delivered = true;
	position = pcm.ops[substream->stream]->pointer(substream);
	base = runtime->hw_ptr_base;
	updated = base + position;
	if (runtime->hw_ptr_interrupt + runtime->period_size > updated &&
	    jiffies - runtime->hw_ptr_jiffies >
		    runtime->hw_ptr_buffer_jiffies / 2 + 1) {
		base += runtime->buffer_size;
		updated = base + position;
	} else if (updated < runtime->status->hw_ptr) {
		base += runtime->buffer_size;
		updated = base + position;
	}
	assert(updated >= runtime->status->hw_ptr &&
	       updated < runtime->boundary);
	delta = updated - runtime->status->hw_ptr;
	if (delta >= runtime->buffer_size + runtime->period_size)
		return;
	if (updated == runtime->status->hw_ptr) {
		runtime->hw_ptr_jiffies = jiffies;
		return;
	}
	delta = updated - runtime->hw_ptr_interrupt;
	runtime->hw_ptr_interrupt += delta - delta % runtime->period_size;
	runtime->hw_ptr_base = base;
	runtime->status->hw_ptr = updated;
	runtime->hw_ptr_jiffies = jiffies;
	assert(updated >= runtime->control->appl_ptr);
	available = updated - runtime->control->appl_ptr;
	if (available >= runtime->stop_threshold) {
		fprintf(stderr,
			"ALSA period consumer reported XRUN: avail=%lu\n",
			available);
		assert(snd_pcm_stop(substream, SNDRV_PCM_STATE_XRUN) == 0);
	}
}

void snd_pcm_drain_done(struct snd_pcm_substream *substream)
{
	abort();
}

int snd_pcm_suspend_all(struct snd_pcm *registered)
{
	abort();
}

int snd_ctl_boolean_mono_info(struct snd_kcontrol *control,
			      struct snd_ctl_elem_info *info)
{
	abort();
}

int snd_ctl_enum_info(struct snd_ctl_elem_info *info, unsigned int count,
		      unsigned int items, const char *const *names)
{
	abort();
}

struct snd_kcontrol *snd_ctl_new1(const struct snd_kcontrol_new *control,
				  void *data)
{
	static struct snd_kcontrol ignored;

	return &ignored;
}

int snd_ctl_add(struct snd_card *registered, struct snd_kcontrol *control)
{
	return 0;
}

struct ums9117_audio *ums9117_audio_create(struct platform_device *pdev)
{
	return &digital;
}

int ums9117_audio_prepare(struct ums9117_audio *audio, unsigned int rate)
{
	assert(audio == &digital && rate == 48000);
	return 0;
}

int ums9117_audio_prepare_fm(struct ums9117_audio *audio, bool external)
{
	abort();
}

void ums9117_audio_start(struct ums9117_audio *audio)
{
	audio->playback_running = true;
	audio->playback_starts++;
}

void ums9117_audio_stop(struct ums9117_audio *audio)
{
	audio->playback_running = false;
}

void ums9117_audio_release(struct ums9117_audio *audio)
{
	assert(!audio->playback_running);
}

int ums9117_audio_queued(struct ums9117_audio *audio)
{
	return audio->queued;
}

void ums9117_audio_write(struct ums9117_audio *audio, u16 left, u16 right)
{
	assert(audio->queued < 320);
	audio->queued++;
	if (!left && !right)
		return;
	CHECK(left == 0x1234 && right == 0xabcd);
	audio->playback_pairs++;
}

int ums9117_audio_prepare_capture(struct ums9117_audio *audio)
{
	return 0;
}

void ums9117_audio_start_capture(struct ums9117_audio *audio)
{
	audio->capture_running = true;
	audio->capture_starts++;
}

void ums9117_audio_stop_capture(struct ums9117_audio *audio)
{
	audio->capture_running = false;
}

void ums9117_audio_release_capture(struct ums9117_audio *audio)
{
	audio->capture_running = false;
}

int ums9117_audio_capture_available(struct ums9117_audio *audio)
{
	return audio->capture_available;
}

u16 ums9117_audio_read_capture(struct ums9117_audio *audio)
{
	assert(audio->capture_available);
	audio->capture_available--;
	return ++audio->captured_samples;
}

void ums9117_audio_report_capture(struct ums9117_audio *audio,
				  const char *reason)
{
	audio->capture_errors++;
	fprintf(stderr, "capture callback reported XRUN: %s\n", reason);
}

void ums9117_audio_set_dac_gain(struct ums9117_audio *audio, u8 left, u8 right)
{
}

int ums9117_audio_set_dac_processing(
	struct ums9117_audio *audio,
	const struct ums9117_audio_dac_processing *processing, bool eq,
	bool alc)
{
	return 0;
}

int ums9117_audio_profile_load(struct device *dev, bool vibration,
			       struct ums9117_audio_profile *profile)
{
	memset(profile, 0, sizeof(*profile));
	return 0;
}

struct ums9117_sc2720_codec *ums9117_sc2720_codec_create(struct device *dev)
{
	return &codec;
}

void ums9117_sc2720_codec_get_volume(struct ums9117_sc2720_codec *instance,
				     unsigned int *left, unsigned int *right)
{
	*left = 1;
	*right = 1;
}

int ums9117_sc2720_codec_set_volume(struct ums9117_sc2720_codec *instance,
				    unsigned int left, unsigned int right)
{
	return 0;
}

int ums9117_sc2720_codec_prepare(struct ums9117_sc2720_codec *instance,
				 unsigned int outputs, u16 pa_word)
{
	instance->playback_prepared = true;
	return 0;
}

int ums9117_sc2720_codec_enable(struct ums9117_sc2720_codec *instance)
{
	assert(instance->playback_prepared);
	return 0;
}

int ums9117_sc2720_codec_stop(struct ums9117_sc2720_codec *instance)
{
	return 0;
}

int ums9117_sc2720_codec_disable(struct ums9117_sc2720_codec *instance)
{
	instance->playback_prepared = false;
	return 0;
}

int ums9117_sc2720_codec_set_outputs(struct ums9117_sc2720_codec *instance,
				     unsigned int outputs, u16 pa_word)
{
	return 0;
}

int ums9117_sc2720_codec_set_speaker_mute(struct ums9117_sc2720_codec *instance,
					  bool mute)
{
	return 0;
}

int ums9117_sc2720_codec_prepare_capture(
	struct ums9117_sc2720_codec *instance,
	enum ums9117_sc2720_capture_source source)
{
	assert(source == UMS9117_SC2720_CAPTURE_INTERNAL);
	instance->capture_prepared = true;
	return 0;
}

int ums9117_sc2720_codec_enable_capture(struct ums9117_sc2720_codec *instance)
{
	assert(instance->capture_prepared);
	return 0;
}

int ums9117_sc2720_codec_stop_capture(struct ums9117_sc2720_codec *instance)
{
	return 0;
}

int ums9117_sc2720_codec_disable_capture(struct ums9117_sc2720_codec *instance)
{
	instance->capture_prepared = false;
	return 0;
}

struct ums9117_jack *ums9117_jack_create(struct device *dev,
					 struct snd_card *registered,
					 struct ums9117_sc2720_codec *instance)
{
	return NULL;
}

void ums9117_jack_start(struct ums9117_jack *jack)
{
}

void ums9117_jack_stop(struct ums9117_jack *jack)
{
}

int ums9117_jack_suspend(struct ums9117_jack *jack)
{
	return 0;
}

void ums9117_jack_resume(struct ums9117_jack *jack)
{
}

static void open_stream(int direction)
{
	struct snd_pcm_substream *substream = &streams[direction];
	struct snd_pcm_runtime *runtime = &runtimes[direction];

	runtime->status = &statuses[direction];
	runtime->control = &controls[direction];
	runtime->rate = 48000;
	runtime->buffer_size = 1920;
	runtime->period_size = 480;
	runtime->boundary = 30720;
	runtime->stop_threshold = 1920;
	runtime->hw_ptr_buffer_jiffies = 40;
	runtime->dma_area = direction == SNDRV_PCM_STREAM_CAPTURE ?
				    (void *)capture_samples :
				    (void *)playback_samples;
	substream->pcm = &pcm;
	substream->runtime = runtime;
	substream->stream = direction;
	assert(pcm.ops[direction]->open(substream) == 0);
	assert(pcm.ops[direction]->prepare(substream) == 0);
	runtime->state = SNDRV_PCM_STATE_PREPARED;
	assert(reset_stream(substream) == 0);
}

static void check_capture_wrap_preserves_playback(void)
{
	struct platform_device device = {};
	struct snd_pcm_substream *capture = &streams[SNDRV_PCM_STREAM_CAPTURE];
	struct snd_pcm_substream *playback =
		&streams[SNDRV_PCM_STREAM_PLAYBACK];
	unsigned int i;

	for (i = 0; i < 1920; i++) {
		playback_samples[i * 2] = 0x1234;
		playback_samples[i * 2 + 1] = 0xabcd;
	}
	assert(pcm_reset_driver->probe(&device) == 0);
	assert(pcm.ops[0] && pcm.ops[1] && service_timer);
	open_stream(SNDRV_PCM_STREAM_PLAYBACK);
	controls[SNDRV_PCM_STREAM_PLAYBACK].appl_ptr = 480;
	CHECK(reset_stream(playback) == 0);
	CHECK(controls[SNDRV_PCM_STREAM_PLAYBACK].appl_ptr == 0);
	CHECK(statuses[SNDRV_PCM_STREAM_PLAYBACK].hw_ptr == 0);
	CHECK(pcm.ops[SNDRV_PCM_STREAM_PLAYBACK]->pointer(playback) == 0);
	controls[SNDRV_PCM_STREAM_PLAYBACK].appl_ptr = 1920;
	assert(pcm.ops[0]->trigger(playback, SNDRV_PCM_TRIGGER_START) == 0);
	runtimes[0].state = SNDRV_PCM_STATE_RUNNING;
	open_stream(SNDRV_PCM_STREAM_CAPTURE);
	assert(pcm.ops[1]->trigger(capture, SNDRV_PCM_TRIGGER_START) == 0);
	runtimes[1].state = SNDRV_PCM_STATE_RUNNING;
	for (i = 0; i < 60; i++)
		run_timer_batch();
	CHECK(capture_samples[0] == 0);
	CHECK(digital.captured_samples == 14400);
	for (i = 0; i < 25; i++) {
		run_timer_batch();
		publish_capture(240);
	}
	check_capture_sample(20400);
	CHECK(statuses[1].hw_ptr == 6000 && controls[1].appl_ptr == 6000);
	CHECK(pcm.ops[1]->pointer(capture) == 240);
	CHECK(pcm.ops[0]->pointer(playback) == 880);
	CHECK(digital.playback_pairs == 20400);
	CHECK(driver_errors == 0);
	interleave_reset = true;
	CHECK(reset_stream(capture) == 0);
	interleave_reset = false;
	check_capture_sample(20640);
	CHECK(pcm.ops[0]->pointer(playback) == 1120);
	CHECK(digital.playback_starts == 2 && digital.capture_starts == 1);
	if (!digital.capture_errors) {
		run_timer_batch();
		check_capture_sample(20880);
	}
	if (!digital.capture_errors) {
		publish_capture(480);
		run_timer_batch();
		check_capture_sample(21120);
		publish_capture(240);
	}
	CHECK(driver_errors == 0);
	assert(pcm.ops[1]->trigger(capture, SNDRV_PCM_TRIGGER_STOP) == 0);
	assert(pcm.ops[1]->close(capture) == 0);
	assert(pcm.ops[0]->trigger(playback, SNDRV_PCM_TRIGGER_STOP) == 0);
	assert(pcm.ops[0]->close(playback) == 0);
	pcm_reset_driver->remove(&device);
	free(device_allocation);
}

static void check_reset_before_pending_capture_notification(void)
{
	struct platform_device device = {};
	struct snd_pcm_substream *capture = &streams[SNDRV_PCM_STREAM_CAPTURE];
	unsigned int i;

	assert(pcm_reset_driver->probe(&device) == 0);
	open_stream(SNDRV_PCM_STREAM_CAPTURE);
	assert(pcm.ops[1]->trigger(capture, SNDRV_PCM_TRIGGER_START) == 0);
	runtimes[1].state = SNDRV_PCM_STATE_RUNNING;
	for (i = 0; i < 60; i++)
		run_timer_batch();
	for (i = 0; i < 6; i++)
		run_timer_batch();
	check_capture_sample(15840);
	CHECK(pcm.ops[1]->pointer(capture) == 1440);
	CHECK(statuses[1].hw_ptr == 0 && controls[1].appl_ptr == 0);
	CHECK(reset_stream(capture) == 0);
	notification_task.stop_when_idle = true;
	CHECK(notification_task.function(notification_task.data) == 0);
	CHECK(notification_task.stop_requested);
	CHECK(capture_period_delivered);
	CHECK(runtimes[1].state == SNDRV_PCM_STATE_RUNNING);
	CHECK(statuses[1].hw_ptr == 1440 && controls[1].appl_ptr == 1440);
	if (snd_pcm_running(capture)) {
		run_timer_batch();
		check_capture_sample(16080);
		publish_capture(240);
		assert(pcm.ops[1]->trigger(capture, SNDRV_PCM_TRIGGER_STOP) ==
		       0);
	}
	CHECK(driver_errors == 0);
	assert(pcm.ops[1]->close(capture) == 0);
	pcm_reset_driver->remove(&device);
	free(device_allocation);
}

int main(int argc, char *argv[])
{
	assert(argc == 2);
	if (!strcmp(argv[1], "capture-wrap"))
		check_capture_wrap_preserves_playback();
	else if (!strcmp(argv[1], "pending-notification"))
		check_reset_before_pending_capture_notification();
	else
		abort();
	if (failures)
		return 1;
	puts("PCM RESET callback checks passed");
	return 0;
}
