/*
 * Copyright 2012 intact
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License as
 * published by the Free Software Foundation; either version 2 of the
 * License, or (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful, but
 * WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the GNU
 * General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program; if not, see <http://www.gnu.org/licenses/>.
 */

#include <bass.h>

#include "ip.h"
#include "xmalloc.h"
#include "comment.h"
#include "debug.h"
#include "options.h"
#include "utils.h"

enum {
	OPTION_INTERPOLATION,
	OPTION_MOD_PLAYBACK_MODE,
	OPTION_RAMPING,
	OPTION_SURROUND,
	NR_OPTIONS
};

static int interpolation = 1; /* linear */
static int mod_playback_mode = 0; /* normal */
static int ramping = 1; /* normal */
static int surround = 0; /* off */

static DWORD bass_flags = 0;

struct bass_private {
	HMUSIC file;
	DWORD  flags;
};

static const char * const interpolation_names[] = {
	"off", "linear", "sinc", NULL
};
static const DWORD interpolation_flags[] = {
	BASS_MUSIC_NONINTER, 0, BASS_MUSIC_SINCINTER
};

static const char * const mod_playback_mode_names[] = {
	"normal", "ft2", "pt1", NULL
};
static const DWORD mod_playback_mode_flags[] = {
	0, BASS_MUSIC_FT2MOD, BASS_MUSIC_PT1MOD
};

static const char * const ramping_names[] = {
	"off", "normal", "sensitive", NULL
};
static const DWORD ramping_flags[] = {
	0, BASS_MUSIC_RAMP, BASS_MUSIC_RAMPS
};

static const char * const surround_names[] = {
	"off", "mode1", "mode2", NULL
};
static const DWORD surround_flags[] = {
	0, BASS_MUSIC_SURROUND, BASS_MUSIC_SURROUND2
};

static void init_bass_flags(void)
{
	DWORD flags = 0;

	flags |= interpolation_flags[interpolation];
	flags |= mod_playback_mode_flags[mod_playback_mode];
	flags |= ramping_flags[ramping];
	flags |= surround_flags[surround];

	bass_flags = flags;
}

static int bass_init(void)
{
	static int inited = 0;

	if (inited)
		return 0;

	if (HIWORD(BASS_GetVersion()) != BASSVERSION) {
		d_print("an incorrect version of BASS was loaded (%x instead of %x)\n", HIWORD(BASS_GetVersion()), BASSVERSION);
		return -IP_ERROR_INTERNAL;
	}

	BASS_SetConfig(BASS_CONFIG_UPDATEPERIOD, 0);

	if (!BASS_Init(0, 44100, 0, 0, NULL)) {
		switch (BASS_ErrorGetCode()) {
			case BASS_ERROR_MEM:
				errno = ENOMEM;
				return -IP_ERROR_ERRNO;
			default:
				d_print("can't initialize device (%d)\n", BASS_ErrorGetCode());
				return -IP_ERROR_INTERNAL;
		}
	}

	init_bass_flags();

	inited = 1;
	return 0;
}

static int bass_open(struct input_plugin_data *ip_data)
{
	HMUSIC file;
	struct bass_private *priv;

	int bi = bass_init();

	if (bi != 0) {
		return bi;
	}

	file = BASS_MusicLoad(FALSE, ip_data->filename, 0, 0, bass_flags | BASS_MUSIC_STOPBACK | BASS_MUSIC_DECODE | BASS_MUSIC_PRESCAN, 0);
	if (file == 0) {
		switch (BASS_ErrorGetCode()) {
			case BASS_ERROR_FILEOPEN:
				errno = ENOENT;
				return -IP_ERROR_ERRNO;
			case BASS_ERROR_MEM:
				errno = ENOMEM;
				return -IP_ERROR_ERRNO;
			case BASS_ERROR_FILEFORM:
				return -IP_ERROR_UNSUPPORTED_FILE_TYPE;
			default:
				d_print("can't play the file (%d)\n", BASS_ErrorGetCode());
				return -IP_ERROR_INTERNAL;
		}
	}

	priv = xnew(struct bass_private, 1);
	priv->file = file;
	priv->flags = bass_flags;

	ip_data->private = priv;
	ip_data->sf = sf_bits(16) | sf_rate(44100) | sf_channels(2) | sf_signed(1);
#ifdef WORDS_BIGENDIAN
	ip_data->sf |= sf_bigendian(1);
#endif
	channel_map_init_stereo(ip_data->channel_map);

	return 0;
}

static int bass_close(struct input_plugin_data *ip_data)
{
	struct bass_private *priv = ip_data->private;

	BASS_MusicFree(priv->file);
	free(ip_data->private);
	ip_data->private = NULL;

	return 0;
}

static int bass_read(struct input_plugin_data *ip_data, char *buffer, int count)
{
	struct bass_private *priv = ip_data->private;
	DWORD flags;
	int rc;

	rc = BASS_ChannelGetData(priv->file, buffer, count);
	if (rc == -1) {
		switch (BASS_ErrorGetCode()) {
			case BASS_ERROR_ENDED:
				return 0;
			default:
				d_print("can't read data (%d)\n", BASS_ErrorGetCode());
				return -IP_ERROR_INTERNAL;
		}
	}

	flags = bass_flags;
	if (flags != priv->flags) {
		priv->flags = flags;
		BASS_ChannelFlags(priv->file, flags, BASS_MUSIC_NONINTER | BASS_MUSIC_SINCINTER | BASS_MUSIC_RAMP | BASS_MUSIC_RAMPS | BASS_MUSIC_SURROUND | BASS_MUSIC_SURROUND2 | BASS_MUSIC_FT2MOD | BASS_MUSIC_PT1MOD);
	}

	return rc;
}

static int bass_seek(struct input_plugin_data *ip_data, double offset)
{
	struct bass_private *priv = ip_data->private;

	if (!BASS_ChannelSetPosition(priv->file, BASS_ChannelSeconds2Bytes(priv->file, offset), BASS_POS_BYTE)) {
		d_print("can't seek (%d)\n", BASS_ErrorGetCode());
		return -IP_ERROR_FUNCTION_NOT_SUPPORTED;
	}

	return 0;
}

static int bass_read_comments(struct input_plugin_data *ip_data, struct keyval **comments)
{
	struct bass_private *priv = ip_data->private;
	GROWING_KEYVALS(c);
	const char *val;

	val = BASS_ChannelGetTags(priv->file, BASS_TAG_MUSIC_NAME);
	if (val && val[0])
		comments_add_const(&c, "title", val);

	val = BASS_ChannelGetTags(priv->file, BASS_TAG_MUSIC_MESSAGE);
	if (val && val[0])
		comments_add_const(&c, "comment", val);

	keyvals_terminate(&c);
	*comments = c.keyvals;

	return 0;
}

static int bass_duration(struct input_plugin_data *ip_data)
{
	struct bass_private *priv = ip_data->private;
	double position;

	position = BASS_ChannelBytes2Seconds(priv->file, BASS_ChannelGetLength(priv->file, BASS_POS_BYTE));
	if (position < 0.0) {
		d_print("can't get duration\n");
		return -IP_ERROR_FUNCTION_NOT_SUPPORTED;
	}

	return position;
}

static long bass_bitrate(struct input_plugin_data *ip_data)
{
	return -IP_ERROR_FUNCTION_NOT_SUPPORTED;
}

static char *bass_codec(struct input_plugin_data *ip_data)
{
	struct bass_private *priv = ip_data->private;
	BASS_CHANNELINFO info;

	if (BASS_ChannelGetInfo(priv->file, &info)) {
		switch (info.ctype) {
			case BASS_CTYPE_MUSIC_IT:
				return xstrdup("it");
			case BASS_CTYPE_MUSIC_MO3:
				return xstrdup("mo3");
			case BASS_CTYPE_MUSIC_MOD:
				return xstrdup("mod");
			case BASS_CTYPE_MUSIC_MTM:
				return xstrdup("mtm");
			case BASS_CTYPE_MUSIC_S3M:
				return xstrdup("s3m");
			case BASS_CTYPE_MUSIC_XM:
				return xstrdup("xm");
		}
	}

	return NULL;
}

static char *bass_codec_profile(struct input_plugin_data *ip_data)
{
	return NULL;
}

static int bass_get_option(int key, char **val)
{
	switch (key) {
		case OPTION_INTERPOLATION:
			*val = xstrdup(interpolation_names[interpolation]);
			break;
		case OPTION_MOD_PLAYBACK_MODE:
			*val = xstrdup(mod_playback_mode_names[mod_playback_mode]);
			break;
		case OPTION_RAMPING:
			*val = xstrdup(ramping_names[ramping]);
			break;
		case OPTION_SURROUND:
			*val = xstrdup(surround_names[surround]);
			break;
		default:
			return -IP_ERROR_NOT_OPTION;
	}
	return 0;
}

static int bass_set_option(int key, const char *val)
{
	switch (key) {
		case OPTION_INTERPOLATION:
			if (!parse_enum(val, 0, N_ELEMENTS(interpolation_flags)-1, interpolation_names, &interpolation)) {
				errno = EINVAL;
				return -IP_ERROR_ERRNO;
			}
			break;
		case OPTION_MOD_PLAYBACK_MODE:
			if (!parse_enum(val, 0, N_ELEMENTS(mod_playback_mode_flags)-1, mod_playback_mode_names, &mod_playback_mode)) {
				errno = EINVAL;
				return -IP_ERROR_ERRNO;
			}
			break;
		case OPTION_RAMPING:
			if (!parse_enum(val, 0, N_ELEMENTS(ramping_flags)-1, ramping_names, &ramping)) {
				errno = EINVAL;
				return -IP_ERROR_ERRNO;
			}
			break;
		case OPTION_SURROUND:
			if (!parse_enum(val, 0, N_ELEMENTS(surround_flags)-1, surround_names, &surround)) {
				errno = EINVAL;
				return -IP_ERROR_ERRNO;
			}
			break;
		default:
			return -IP_ERROR_NOT_OPTION;
	}
	init_bass_flags();
	return 0;
}

const struct input_plugin_ops ip_ops = {
	.open = bass_open,
	.close = bass_close,
	.read = bass_read,
	.seek = bass_seek,
	.read_comments = bass_read_comments,
	.duration = bass_duration,
	.bitrate = bass_bitrate,
	.bitrate_current = bass_bitrate,
	.codec = bass_codec,
	.codec_profile = bass_codec_profile,
	.get_option = bass_get_option,
	.set_option = bass_set_option
};

const int ip_priority = 55;

const char * const ip_extensions[] = {
	"it",
	"mo3",
	"mod",
	"mtm",
	"s3m",
	"umx",
	"xm",
	NULL
};

const char * const ip_mime_types[] = {
	NULL
};

const char * const ip_options[] = {
	"interpolation",
	"mod_playback_mode",
	"ramping",
	"surround",
	NULL
};
