/*
    Copyright (C) 2026 Ardour contributors

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation; either version 2 of the License, or
    (at your option) any later version.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License along
    with this program; if not, write to the Free Software Foundation, Inc.,
    51 Franklin Street, Fifth Floor, Boston, MA 02110-1301 USA.
*/

#include "aaudio_device.h"

#include <aaudio/AAudio.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

struct AAudioDevice {
	AAudioStream* stream;
	int32_t       sample_rate;
	int32_t       channel_count;
	int32_t       frames_per_burst;
	int32_t       buffer_capacity;
	int32_t       device_id;
	char          last_error[256];
};

static void
aa_device_set_error (AAudioDevice* device, const char* text)
{
	if (device == NULL) {
		return;
	}
	if (text == NULL) {
		text = "unknown error";
	}
	snprintf (device->last_error, sizeof (device->last_error), "%s", text);
}

static AAudioDevice*
aa_device_open_common (int32_t sample_rate, int32_t channel_count, int32_t frames_per_burst, aaudio_direction_t direction)
{
	AAudioDevice* device = (AAudioDevice*) calloc (1, sizeof (AAudioDevice));

	if (device == NULL) {
		return NULL;
	}

	AAudioStreamBuilder* builder = AAudio_createStreamBuilder ();

	if (builder == NULL) {
		aa_device_set_error (device, "AAudio_createStreamBuilder() failed");
		return device;
	}

	/* device id 0 is the default device of the given direction */
	AAudioStreamBuilder_setDeviceId (builder, 0);
	AAudioStreamBuilder_setDirection (builder, direction);
	AAudioStreamBuilder_setSampleRate (builder, sample_rate);
	AAudioStreamBuilder_setChannelCount (builder, channel_count);
	AAudioStreamBuilder_setFormat (builder, AAUDIO_FORMAT_PCM_FLOAT);
	AAudioStreamBuilder_setPerformanceMode (builder, AAUDIO_PERFORMANCE_MODE_LOW_LATENCY);
	AAudioStreamBuilder_setSharingMode (builder, AAUDIO_SHARING_MODE_SHARED);

	/* The stream is used in blocking mode, so the burst size is chosen by the
	 * device and reported by AAudioStream_getFramesPerBurst().  The request is
	 * kept for the API, but it is only a hint. */
	(void) frames_per_burst;

	aaudio_result_t result = AAUDIO_OK;
	AAudioStream* stream = AAudioStreamBuilder_openStream (builder, &result);

	AAudioStreamBuilder_delete (builder);

	if (stream == NULL) {
		aa_device_set_error (device, AAudio_convert_result_to_text (result));
		return device;
	}

	device->stream             = stream;
	device->sample_rate        = AAudioStream_getSampleRate (stream);
	device->channel_count      = AAudioStream_getChannelCount (stream);
	device->frames_per_burst   = AAudioStream_getFramesPerBurst (stream);
	device->buffer_capacity    = AAudioStream_getBufferCapacityInFrames (stream);
	device->device_id          = AAudioStream_getDeviceId (stream);

	return device;
}

AAudioDevice*
aa_device_open (int32_t sample_rate, int32_t channel_count, int32_t frames_per_burst)
{
	return aa_device_open_common (sample_rate, channel_count, frames_per_burst, AAUDIO_DIRECTION_OUTPUT);
}

AAudioDevice*
aa_device_open_input (int32_t sample_rate, int32_t channel_count, int32_t frames_per_burst)
{
	return aa_device_open_common (sample_rate, channel_count, frames_per_burst, AAUDIO_DIRECTION_INPUT);
}

void
aa_device_close (AAudioDevice* device)
{
	if (device == NULL) {
		return;
	}
	if (device->stream != NULL) {
		AAudioStream_close (device->stream);
		device->stream = NULL;
	}
	free (device);
}

int
aa_device_start (AAudioDevice* device)
{
	if (device == NULL || device->stream == NULL) {
		return AAUDIO_DEVICE_ERROR;
	}
	aaudio_result_t result = AAudioStream_requestStart (device->stream);

	if (result != AAUDIO_OK) {
		aa_device_set_error (device, AAudio_convert_result_to_text (result));
		return AAUDIO_DEVICE_ERROR;
	}
	return AAUDIO_DEVICE_OK;
}

int
aa_device_stop (AAudioDevice* device)
{
	if (device == NULL || device->stream == NULL) {
		return AAUDIO_DEVICE_ERROR;
	}
	aaudio_result_t result = AAudioStream_requestStop (device->stream);

	if (result != AAUDIO_OK) {
		aa_device_set_error (device, AAudio_convert_result_to_text (result));
		return AAUDIO_DEVICE_ERROR;
	}
	return AAUDIO_DEVICE_OK;
}

static int32_t
aa_device_map_error (AAudioDevice* device, aaudio_result_t result)
{
	switch (result) {
	case AAUDIO_ERROR_DISCONNECTED:
		aa_device_set_error (device, "stream disconnected (the device was taken away)");
		return AAUDIO_DEVICE_DISCONNECTED;
	case AAUDIO_ERROR_INTERRUPTED:
		aa_device_set_error (device, "stream interrupted");
		return AAUDIO_DEVICE_INTERRUPTED;
	default:
		/* the text of the AAudio result is more informative than any
		 * mapping of it onto the status codes of this wrapper */
		aa_device_set_error (device, AAudio_convert_result_to_text (result));
		return AAUDIO_DEVICE_ERROR;
	}
}

int32_t
aa_device_read (AAudioDevice* device, void* buffer, int32_t frames, int64_t timeout_nanos)
{
	if (device == NULL || device->stream == NULL || buffer == NULL || frames <= 0) {
		return AAUDIO_DEVICE_ERROR;
	}
	aaudio_result_t result = AAudioStream_read (device->stream, buffer, (int32_t) frames, timeout_nanos);

	if (result < 0) {
		return aa_device_map_error (device, result);
	}
	return (int32_t) result;
}

int32_t
aa_device_write (AAudioDevice* device, const void* buffer, int32_t frames, int64_t timeout_nanos)
{
	if (device == NULL || device->stream == NULL || buffer == NULL || frames <= 0) {
		return AAUDIO_DEVICE_ERROR;
	}
	aaudio_result_t result = AAudioStream_write (device->stream, buffer, (int32_t) frames, timeout_nanos);

	if (result < 0) {
		return aa_device_map_error (device, result);
	}
	return (int32_t) result;
}

int32_t
aa_device_get_sample_rate (const AAudioDevice* device)
{
	return device ? device->sample_rate : 0;
}

int32_t
aa_device_get_channel_count (const AAudioDevice* device)
{
	return device ? device->channel_count : 0;
}

int32_t
aa_device_get_frames_per_burst (const AAudioDevice* device)
{
	return device ? device->frames_per_burst : 0;
}

int32_t
aa_device_get_buffer_capacity (const AAudioDevice* device)
{
	return device ? device->buffer_capacity : 0;
}

int32_t
aa_device_get_format (const AAudioDevice* device)
{
	if (device == NULL || device->stream == NULL) {
		return AAUDIO_FORMAT_INVALID;
	}
	return AAudioStream_getFormat (device->stream);
}

int32_t
aa_device_get_device_id (const AAudioDevice* device)
{
	return device ? device->device_id : 0;
}

int32_t
aa_device_get_xrun_count (const AAudioDevice* device)
{
	if (device == NULL || device->stream == NULL) {
		return 0;
	}
	return AAudioStream_getXRunCount (device->stream);
}

const char*
aa_device_get_last_error (const AAudioDevice* device)
{
	if (device == NULL) {
		return "no device";
	}
	return device->last_error;
}

int
aa_device_supports_device_list (void)
{
#if defined (__ANDROID_API__) && __ANDROID_API__ >= 30
	return 1;
#else
	return 0;
#endif
}

int
aa_device_get_device_list (char* buffer, int32_t buffer_size)
{
	if (buffer == NULL || buffer_size <= 0) {
		return 0;
	}
	buffer[0] = '\0';

#if defined (__ANDROID_API__) && __ANDROID_API__ >= 30
	AAudioSystem* system = AAudioSystem_getInstance ();

	if (system == NULL) {
		return 0;
	}

	int32_t* ids = NULL;
	int32_t  count = AAudioSystem_getDeviceIds (system, &ids);

	if (count <= 0 || ids == NULL) {
		return 0;
	}

	int written = 0;

	for (int32_t n = 0; n < count; ++n) {
		AAudioDeviceInfo info;
		memset (&info, 0, sizeof (info));

		if (AAudioSystem_getDeviceInfo (system, ids[n], &info) != AAUDIO_OK) {
			continue;
		}

		const char* name = AAudioDeviceInfo_getDeviceName (&info);

		if (name == NULL) {
			name = "(unnamed)";
		}

		int n_written = snprintf (buffer + written, (size_t) (buffer_size - written),
					  "%d: %s\n", (int) info.id, name);

		if (n_written < 0 || n_written >= buffer_size - written) {
			break;
		}
		written += n_written;
	}

	AAudioSystem_releaseDeviceIds (system, ids);

	return written;
#else
	/* AAudio only exposes device enumeration from API 30 on */
	return 0;
#endif
}
