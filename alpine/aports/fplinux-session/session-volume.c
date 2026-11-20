/* SPDX-License-Identifier: GPL-2.0-only */
#include "session-core-internal.h"

#include <stdio.h>
#include <string.h>

#include "fplinux-audio-protocol.h"

#define FPLINUX_SESSION_VOLUME_REPEAT_DELAY_MS 500U
#define FPLINUX_SESSION_VOLUME_REPEAT_MS 200U
#define FPLINUX_SESSION_INDICATOR_MS 1500U

enum volume_setting_operation {
	VOLUME_SETTING_SET,
	VOLUME_SETTING_PREVIEW,
	VOLUME_SETTING_STEP,
	VOLUME_SETTING_COMMIT,
	VOLUME_SETTING_PERSISTENT_STEP,
};

struct volume_setting {
	const char *name;
	enum fplinux_session_audio_class class;
	enum volume_setting_operation operation;
};

static const struct volume_setting volume_settings[] = {
	{ "volume", FPLINUX_SESSION_AUDIO_MEDIA, VOLUME_SETTING_SET },
	{ "volume_preview", FPLINUX_SESSION_AUDIO_MEDIA,
	  VOLUME_SETTING_PREVIEW },
	{ "volume_step", FPLINUX_SESSION_AUDIO_MEDIA, VOLUME_SETTING_STEP },
	{ "volume_commit", FPLINUX_SESSION_AUDIO_MEDIA, VOLUME_SETTING_COMMIT },
	{ "media_volume_step", FPLINUX_SESSION_AUDIO_MEDIA,
	  VOLUME_SETTING_PERSISTENT_STEP },
	{ "alarm_volume", FPLINUX_SESSION_AUDIO_ALARM, VOLUME_SETTING_SET },
	{ "alarm_volume_preview", FPLINUX_SESSION_AUDIO_ALARM,
	  VOLUME_SETTING_PREVIEW },
	{ "alarm_volume_commit", FPLINUX_SESSION_AUDIO_ALARM,
	  VOLUME_SETTING_COMMIT },
};

static const struct volume_setting *find_setting(const char *name)
{
	for (size_t i = 0;
	     i < sizeof(volume_settings) / sizeof(volume_settings[0]); ++i)
		if (!strcmp(name, volume_settings[i].name))
			return &volume_settings[i];
	return NULL;
}

bool fplinux_session_volume_has_setting(const char *name)
{
	return find_setting(name) != NULL;
}

static void volume_error(struct process *process,
			 const struct fplinux_ui_session_message *message,
			 const char *reason)
{
	fplinux_session_reply_with_serial(process, FPLINUX_UI_SESSION_ERROR,
					  message->name, message->value, reason,
					  message->serial);
}

static enum fplinux_system_level_kind
volume_level_kind(enum fplinux_session_audio_class class)
{
	return class == FPLINUX_SESSION_AUDIO_MEDIA ?
		       FPLINUX_SYSTEM_LEVEL_MEDIA :
		       FPLINUX_SYSTEM_LEVEL_ALARM;
}

static void read_level(const struct fplinux_session *session,
		       enum fplinux_session_audio_class class,
		       struct fplinux_system_level_state *state)
{
	memset(state, 0, sizeof(*state));
	state->maximum = FPLINUX_AUDIO_VOLUME_MAX;

	if (!session->has_audio || session->audio.fd < 0)
		state->availability = FPLINUX_SYSTEM_LEVEL_UNAVAILABLE;
	else if (fplinux_session_audio_volume_applied(&session->audio, class,
						      &state->value))
		state->availability = FPLINUX_SYSTEM_LEVEL_READY;
	else
		state->availability = FPLINUX_SYSTEM_LEVEL_UNKNOWN;
	if (state->availability != FPLINUX_SYSTEM_LEVEL_UNAVAILABLE)
		state->pending = fplinux_session_audio_volume_pending(
					 &session->audio, class) ||
				 session->volume.controls[class].key_pending;
}

void fplinux_session_volume_read_levels(struct fplinux_session *session,
					struct fplinux_system_levels *levels)
{
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		read_level(session, i, &levels->level[volume_level_kind(i)]);
}

static enum fplinux_session_audio_class
volume_key_class(const struct fplinux_session *session)
{
	return fplinux_session_clock_ringing(&session->clock) ?
		       FPLINUX_SESSION_AUDIO_ALARM :
		       FPLINUX_SESSION_AUDIO_MEDIA;
}

static bool key_context_matches(struct fplinux_session *session,
				const struct volume_keys *keys)
{
	const struct volume_control *control =
		&session->volume.controls[keys->class];

	return keys->focus_pid == fplinux_session_focused_pid(session) &&
	       keys->quick_settings ==
		       fplinux_session_quick_settings_shown(session) &&
	       keys->class == volume_key_class(session) &&
	       keys->preview == control->preview_active &&
	       (fplinux_session_power_is_visible(&session->power) ||
		(keys->class == FPLINUX_SESSION_AUDIO_ALARM &&
		 fplinux_session_clock_keys_eligible(session)));
}

static bool indicator_held(struct fplinux_session *session)
{
	for (unsigned int i = 0; i < FPLINUX_SESSION_KEY_SOURCE_COUNT; ++i) {
		const struct volume_keys *keys = &session->volume.keys[i];

		if (keys->repeat_code &&
		    keys->held[keys->repeat_code == KEY_VOLUMEUP] &&
		    volume_level_kind(keys->class) ==
			    session->volume.indicator.kind &&
		    key_context_matches(session, keys))
			return true;
	}
	return false;
}

static void start_indicator(struct fplinux_session *session,
			    enum fplinux_session_audio_class class,
			    bool alarm_keys)
{
	struct fplinux_session_volume *volume = &session->volume;
	enum fplinux_system_level_kind kind = volume_level_kind(class);
	struct fplinux_system_level_state *level = &volume->indicator.level;

	if (!volume->indicator_active || volume->indicator.kind != kind) {
		read_level(session, class, level);
		if (level->availability != FPLINUX_SYSTEM_LEVEL_READY &&
		    session->audio.volume[class].confirmed) {
			level->availability = FPLINUX_SYSTEM_LEVEL_READY;
			level->value = session->audio.volume[class].applied;
		}
	}
	volume->indicator.kind = kind;
	volume->indicator.warning = false;
	volume->indicator_alarm_keys = alarm_keys;
	volume->indicator_focus_pid = fplinux_session_focused_pid(session);
	volume->indicator_active = true;
	volume->indicator_deadline_ms =
		fplinux_session_now_ms(session) + FPLINUX_SESSION_INDICATOR_MS;
}

bool fplinux_session_volume_advance_indicator(struct fplinux_session *session,
					      uint64_t time, uint64_t *earliest,
					      bool notice, bool softkeys)
{
	struct fplinux_session_volume *volume = &session->volume;
	struct fplinux_session_indicator *indicator = &volume->indicator;
	const struct fplinux_system_level_state *level =
		&session->system_levels.level[indicator->kind];
	bool held = indicator_held(session);

	if (volume->indicator_active &&
	    (!session->ready ||
	     session->power.display == FPLINUX_UI_SESSION_DISPLAY_OFF ||
	     session->light_pending || notice ||
	     volume->indicator_focus_pid !=
		     fplinux_session_focused_pid(session) ||
	     (session->power.locked &&
	      !(volume->indicator_alarm_keys &&
		fplinux_session_clock_keys_eligible(session))) ||
	     (!held && time >= volume->indicator_deadline_ms)))
		volume->indicator_active = false;
	if (volume->indicator_active) {
		if (level->availability == FPLINUX_SYSTEM_LEVEL_READY ||
		    indicator->level.availability != FPLINUX_SYSTEM_LEVEL_READY)
			indicator->level = *level;
		if (level->availability == FPLINUX_SYSTEM_LEVEL_UNAVAILABLE)
			indicator->warning = true;
		if (!held &&
		    (!*earliest || volume->indicator_deadline_ms < *earliest))
			*earliest = volume->indicator_deadline_ms;
	}
	indicator->visible = volume->indicator_active;
	indicator->softkeys = softkeys;
	if (memcmp(&volume->last_indicator, indicator, sizeof(*indicator))) {
		volume->last_indicator = *indicator;
		return true;
	}
	return false;
}

static void end_volume_preview(struct fplinux_session *session,
			       enum fplinux_session_audio_class class)
{
	struct volume_control *control = &session->volume.controls[class];

	if (!control->preview_active)
		return;
	control->preview_active = false;
	if (control->request.pending && control->request.preview)
		control->request.pending = false;
	fplinux_session_audio_set_volume(&session->audio, class,
					 control->preview_restore,
					 fplinux_session_now_ms(session));
}

static const char *volume_name(enum fplinux_session_audio_class class,
			       bool preview)
{
	if (class == FPLINUX_SESSION_AUDIO_ALARM)
		return preview ? "alarm_volume_preview" : "alarm_volume";
	return preview ? "volume_preview" : "volume";
}

static void set_key_volume(struct fplinux_session *session,
			   enum fplinux_session_audio_class class,
			   unsigned int level)
{
	struct volume_control *control = &session->volume.controls[class];
	struct setting_request *request = &control->request;

	if (request->pending && request->pid && !request->preview &&
	    !request->step) {
		control->key_pending = true;
		control->key_level = level;
		return;
	}
	if (!control->preview_active && !request->pending)
		*request = (struct setting_request){
			.restore_level = session->audio.volume[class].desired,
		};
	if (control->preview_active) {
		request->restore_level = control->preview_restore;
		request->preview = true;
	} else if (!request->pending || !request->step) {
		request->pid = 0;
		request->token = 0;
		request->serial = 0;
		request->preview = false;
	}
	request->level = level;
	request->save = !request->preview;
	request->feedback = true;
	request->audio_fd = session->audio.fd;
	request->pending = true;
	if (!request->step)
		fplinux_session_copy_value(request->name, sizeof(request->name),
					   volume_name(class,
						       request->preview));
	fplinux_session_audio_set_volume(&session->audio, class, level,
					 fplinux_session_now_ms(session));
}

static unsigned int stepped_volume(unsigned int level, int step)
{
	if (step > 0)
		return level < FPLINUX_AUDIO_VOLUME_MAX ? level + 1 : level;
	return level ? level - 1 : 0;
}

static bool
dispatch_volume_step(struct fplinux_session *session,
		     enum fplinux_session_audio_class class, int step,
		     struct process *process,
		     const struct fplinux_ui_session_message *message)
{
	struct volume_control *control = &session->volume.controls[class];
	struct setting_request *request = &control->request;
	struct setting_request *recipient = request;
	unsigned int applied;
	unsigned int level = control->key_pending ?
				     control->key_level :
				     session->audio.volume[class].desired;
	unsigned int next = stepped_volume(level, step);
	char current[16];

	if (process && control->preview_active) {
		volume_error(process, message, "busy");
		return false;
	}
	start_indicator(session, class,
			!process && class == FPLINUX_SESSION_AUDIO_ALARM);
	if (!fplinux_session_audio_volume_applied(&session->audio, class,
						  &applied)) {
		session->volume.indicator.warning = true;
		if (process)
			volume_error(process, message, "unavailable");
		return false;
	}
	if (process) {
		if (request->pending && !request->step)
			recipient = &control->step_request;
		if (recipient->pending && recipient->step) {
			struct process *previous =
				fplinux_session_setting_requester(session,
								  recipient);

			if (previous) {
				snprintf(current, sizeof(current), "%u",
					 recipient->level);
				fplinux_session_reply_with_serial(
					previous, FPLINUX_UI_SESSION_ERROR,
					recipient->name, current, "superseded",
					recipient->serial);
			}
		}
	}
	if (next == level && !request->pending && !control->key_pending) {
		if (process) {
			snprintf(current, sizeof(current), "%u", applied);
			fplinux_session_setting_reply(session, process,
						      message->name, current,
						      message->serial, false);
		}
		return false;
	}
	if (next != level)
		set_key_volume(session, class, next);
	if (process) {
		if (recipient == &control->step_request) {
			control->key_pending = true;
			control->key_level = next;
		}
		fplinux_session_remember_setting_request(recipient, process,
							 message);
		recipient->level = next;
		recipient->step = true;
		recipient->feedback = true;
		fplinux_session_copy_value(recipient->name,
					   sizeof(recipient->name),
					   message->name);
	}
	return next != level;
}

static bool volume_key_step(struct fplinux_session *session, unsigned int code)
{
	return dispatch_volume_step(session, volume_key_class(session),
				    code == KEY_VOLUMEUP ? 1 : -1, NULL, NULL);
}

static void volume_setting(struct fplinux_session *session,
			   struct process *process,
			   const struct fplinux_ui_session_message *message,
			   enum fplinux_session_audio_class class,
			   enum volume_setting_operation operation)
{
	struct fplinux_session_settings next = session->settings;
	struct volume_control *control = &session->volume.controls[class];
	struct setting_request *request = &control->request;
	unsigned int applied;
	unsigned int level;
	char current[16];
	bool persistent_step = operation == VOLUME_SETTING_PERSISTENT_STEP;
	bool step = operation == VOLUME_SETTING_STEP || persistent_step;
	bool commit = operation == VOLUME_SETTING_COMMIT;
	bool preview = operation == VOLUME_SETTING_PREVIEW ||
		       operation == VOLUME_SETTING_STEP;
	bool rollback = operation == VOLUME_SETTING_PREVIEW &&
			!message->value[0] && control->preview_active;

	if ((step && strcmp(message->value, "-1") &&
	     strcmp(message->value, "1")) ||
	    (commit && message->value[0])) {
		volume_error(process, message, "invalid-value");
		return;
	}
	if (persistent_step) {
		dispatch_volume_step(session, class,
				     !strcmp(message->value, "1") ? 1 : -1,
				     process, message);
		return;
	}
	if (!step && !commit && message->value[0] &&
	    !fplinux_session_settings_parse(
		    &next, session->options.languages_dir,
		    class == FPLINUX_SESSION_AUDIO_MEDIA ?
			    FPLINUX_SESSION_SETTING_VOLUME :
			    FPLINUX_SESSION_SETTING_ALARM_VOLUME,
		    message->value)) {
		volume_error(process, message, "invalid-value");
		return;
	}
	if (request->pending && request->save &&
	    (message->value[0] || commit)) {
		volume_error(process, message, "busy");
		return;
	}
	if (operation == VOLUME_SETTING_PREVIEW && !message->value[0]) {
		end_volume_preview(session, class);
		preview = false;
	}
	if (!fplinux_session_audio_volume_applied(&session->audio, class,
						  &applied)) {
		volume_error(process, message, "unavailable");
		return;
	}
	if (!message->value[0] && !rollback && !commit) {
		snprintf(current, sizeof(current), "%u", applied);
		fplinux_session_setting_reply(session, process,
					      volume_name(class, false),
					      current, message->serial, false);
		return;
	}
	if (step)
		level = stepped_volume(session->audio.volume[class].desired,
				       !strcmp(message->value, "1") ? 1 : -1);
	else
		level = commit || !message->value[0] ?
				session->audio.volume[class].desired :
			class == FPLINUX_SESSION_AUDIO_MEDIA ?
				next.volume :
				next.alarm_volume;
	if (message->value[0] && preview && !control->preview_active) {
		control->preview_active = true;
		control->preview_launcher = process == &session->launcher;
		/* A previous preview may still be restoring asynchronously. */
		control->preview_restore = session->audio.volume[class].desired;
	}
	request->restore_level =
		control->preview_active ? control->preview_restore : applied;
	request->level = level;
	if (rollback)
		request->restore_level = request->level;
	request->save = commit || (message->value[0] && !preview);
	request->preview = preview;
	request->feedback = false;
	request->step = false;
	request->audio_fd = session->audio.fd;
	fplinux_session_remember_setting_request(request, process, message);
	fplinux_session_copy_value(request->name, sizeof(request->name),
				   volume_name(class, preview));
	if (request->save)
		control->preview_active = false;
	fplinux_session_audio_set_volume(&session->audio, class, request->level,
					 fplinux_session_now_ms(session));
}

static void finish_volume_request(struct fplinux_session *session,
				  enum fplinux_session_audio_class class)
{
	struct volume_control *control = &session->volume.controls[class];
	struct setting_request *request = &control->request;
	struct process *process;
	unsigned int applied;
	char current[16];
	const char *name;

	if (!request->pending)
		return;
	process = fplinux_session_setting_requester(session, request);
	if (class == FPLINUX_SESSION_AUDIO_MEDIA && !request->pid &&
	    fplinux_session_quick_settings_shown(session) &&
	    session->launcher.control_fd >= 0)
		process = &session->launcher;
	name = request->name;
	if (session->audio.fd != request->audio_fd || session->audio.fd < 0) {
		struct process *step_process =
			fplinux_session_setting_requester(
				session, &control->step_request);

		request->pending = false;
		control->key_pending = false;
		if (control->step_request.pending && step_process)
			fplinux_session_reply_with_serial(
				step_process, FPLINUX_UI_SESSION_ERROR,
				control->step_request.name, "", "unavailable",
				control->step_request.serial);
		control->step_request.pending = false;
		if (request->feedback && session->volume.indicator_active &&
		    session->volume.indicator.kind == volume_level_kind(class))
			session->volume.indicator.warning = true;
		if (request->preview)
			control->preview_active = false;
		fplinux_session_audio_set_volume(
			&session->audio, class, request->restore_level,
			fplinux_session_now_ms(session));
		if (process) {
			snprintf(current, sizeof(current), "%u",
				 request->level);
			fplinux_session_reply_with_serial(
				process, FPLINUX_UI_SESSION_ERROR, name,
				current, "unavailable", request->serial);
		}
		return;
	}
	if (fplinux_session_audio_volume_pending(&session->audio, class) ||
	    !fplinux_session_audio_volume_applied(&session->audio, class,
						  &applied))
		return;
	request->pending = false;
	if (request->save) {
		if (class == FPLINUX_SESSION_AUDIO_MEDIA) {
			session->settings.volume = applied;
			session->settings.has_volume = true;
		} else {
			session->settings.alarm_volume = applied;
			session->settings.has_alarm_volume = true;
		}
		session->settings_retry_delay_ms = 0;
		fplinux_session_save_settings(session);
		if (session->settings_storage.error &&
		    session->volume.indicator_active &&
		    session->volume.indicator.kind == volume_level_kind(class))
			session->volume.indicator.warning = true;
	}
	if (process) {
		snprintf(current, sizeof(current), "%u", applied);
		fplinux_session_setting_reply(session, process, name, current,
					      request->serial, request->save);
	}
	if (control->key_pending) {
		struct setting_request queued = control->step_request;
		struct process *step_process =
			fplinux_session_setting_requester(session, &queued);

		control->key_pending = false;
		control->step_request.pending = false;
		if (control->key_level != applied) {
			set_key_volume(session, class, control->key_level);
			if (queued.pending && step_process) {
				request->pid = queued.pid;
				request->token = queued.token;
				request->serial = queued.serial;
				request->step = true;
				fplinux_session_copy_value(
					request->name, sizeof(request->name),
					queued.name);
			}
		} else if (queued.pending && step_process) {
			snprintf(current, sizeof(current), "%u", applied);
			fplinux_session_setting_reply(session, step_process,
						      queued.name, current,
						      queued.serial, false);
		}
	}
}

void fplinux_session_volume_repeat_keys(struct fplinux_session *session,
					uint64_t time, uint64_t *earliest)
{
	for (unsigned int i = 0; i < FPLINUX_SESSION_KEY_SOURCE_COUNT; ++i) {
		struct volume_keys *keys = &session->volume.keys[i];

		if (!keys->repeat_ms)
			continue;
		if (!key_context_matches(session, keys) ||
		    !fplinux_session_audio_volume_applied(&session->audio,
							  keys->class, NULL)) {
			keys->repeat_ms = 0;
			keys->repeat_code = 0;
			continue;
		}
		if (time >= keys->repeat_ms) {
			if (!volume_key_step(session, keys->repeat_code)) {
				keys->repeat_ms = 0;
				continue;
			}
			/* A delayed dispatch takes one step, without a catch-up burst. */
			keys->repeat_ms =
				time + FPLINUX_SESSION_VOLUME_REPEAT_MS;
		}
		if (!*earliest || keys->repeat_ms < *earliest)
			*earliest = keys->repeat_ms;
	}
}

bool fplinux_session_volume_key(struct fplinux_session *session,
				enum fplinux_session_key_source source,
				unsigned int code, bool pressed, bool deliver)
{
	struct volume_keys *keys = &session->volume.keys[source];
	unsigned int slot;

	if (code != KEY_VOLUMEUP && code != KEY_VOLUMEDOWN)
		return deliver;
	slot = code == KEY_VOLUMEUP;
	if (!pressed) {
		keys->held[slot] = false;
		if (keys->repeat_code == code) {
			keys->repeat_ms = 0;
			keys->repeat_code = 0;
			session->volume.indicator_deadline_ms =
				fplinux_session_now_ms(session) +
				FPLINUX_SESSION_INDICATOR_MS;
		}
	} else if (!keys->held[slot]) {
		keys->held[slot] = true;
		keys->repeat_ms = 0;
		if (deliver) {
			bool changed = volume_key_step(session, code);

			keys->repeat_code = code;
			keys->focus_pid = fplinux_session_focused_pid(session);
			keys->quick_settings =
				fplinux_session_quick_settings_shown(session);
			keys->class = volume_key_class(session);
			keys->preview = session->volume.controls[keys->class]
						.preview_active;
			if (changed)
				keys->repeat_ms =
					fplinux_session_now_ms(session) +
					FPLINUX_SESSION_VOLUME_REPEAT_DELAY_MS;
		}
	}
	return false;
}

bool fplinux_session_volume_setting(
	struct fplinux_session *session, struct process *process,
	const struct fplinux_ui_session_message *message)
{
	const struct volume_setting *setting = find_setting(message->name);

	if (!setting)
		return false;
	volume_setting(session, process, message, setting->class,
		       setting->operation);
	return true;
}

void fplinux_session_volume_load(struct fplinux_session *session)
{
	fplinux_session_audio_set_volume(&session->audio,
					 FPLINUX_SESSION_AUDIO_MEDIA,
					 session->settings.volume,
					 fplinux_session_now_ms(session));
	fplinux_session_audio_set_volume(&session->audio,
					 FPLINUX_SESSION_AUDIO_ALARM,
					 session->settings.alarm_volume,
					 fplinux_session_now_ms(session));
}

void fplinux_session_volume_finish_requests(struct fplinux_session *session)
{
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		finish_volume_request(session, i);
}

void fplinux_session_volume_end_app_previews(struct fplinux_session *session)
{
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		if (!session->volume.controls[i].preview_launcher)
			end_volume_preview(session, i);
}

void fplinux_session_volume_end_quick_settings_preview(
	struct fplinux_session *session)
{
	if (session->volume.controls[FPLINUX_SESSION_AUDIO_MEDIA]
		    .preview_launcher)
		end_volume_preview(session, FPLINUX_SESSION_AUDIO_MEDIA);
}

void fplinux_session_volume_end_previews(struct fplinux_session *session)
{
	for (unsigned int i = 0; i < FPLINUX_SESSION_AUDIO_CLASSES; ++i)
		end_volume_preview(session, i);
}

void fplinux_session_volume_cancel_input(struct fplinux_session *session,
					 enum fplinux_session_key_source source)
{
	struct volume_keys *keys = &session->volume.keys[source];

	if (keys->repeat_code)
		session->volume.indicator_deadline_ms =
			fplinux_session_now_ms(session) +
			FPLINUX_SESSION_INDICATOR_MS;
	memset(keys, 0, sizeof(*keys));
}
