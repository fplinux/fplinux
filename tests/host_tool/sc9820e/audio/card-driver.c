// SPDX-License-Identifier: GPL-2.0-only
#include <alsa/asoundlib.h>

int qin_test_card_driver(snd_config_t **destination, snd_config_t *root,
			 snd_config_t *source, snd_config_t *private_data);

/* Replace only the control-device query; the configuration evaluator is real. */
int qin_test_card_driver(snd_config_t **destination, snd_config_t *root,
			 snd_config_t *source, snd_config_t *private_data)
{
	const char *driver;
	const char *id;
	int ret;

	(void)root;
	ret = snd_config_get_string(private_data, &driver);
	if (ret < 0)
		return ret;
	ret = snd_config_get_id(source, &id);
	if (ret < 0)
		return ret;
	return snd_config_imake_string(destination, id, driver);
}

SND_DLSYM_BUILD_VERSION(qin_test_card_driver,
			SND_CONFIG_DLSYM_VERSION_EVALUATE);
