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

#ifndef __ardour_aaudio_device_h__
#define __ardour_aaudio_device_h__

/*
 * A thin, Ardour-independent wrapper around Android's AAudio API.
 *
 * It is deliberately plain C with no Ardour and no C++ dependency, for two
 * reasons:
 *
 *   1. the C++ backend (aaudio.cc) only has to deal with the Ardour side of
 *      the problem, and
 *   2. the file can be compiled on its own against the NDK, which is what the
 *      CI job does: it proves that the AAudio calls used here exist and have
 *      the expected signatures, without needing the whole cross-compiled
 *      dependency stack of Ardour.
 */

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct AAudioDevice AAudioDevice;

/* Status codes returned by this wrapper.  Negative values are errors; they
 * are deliberately not aaudio_result_t values, so that a caller can tell an
 * AAudio failure (which is reported through aa_device_get_last_error()) from
 * a plain "nothing happened" answer. */
enum {
	AAUDIO_DEVICE_OK           =  0,
	AAUDIO_DEVICE_ERROR        = -1,  /* the AAudio call failed, see the error text */
	AAUDIO_DEVICE_UNSUPPORTED  = -2,  /* the device refused the requested format */
	AAUDIO_DEVICE_DISCONNECTED = -3,  /* the stream went away (phone call, app stolen it) */
	AAUDIO_DEVICE_INTERRUPTED  = -4,  /* the stream was interrupted and may be resumed */
};

/* Open the default output device.
 *
 * sample_rate and channel_count are a request: the device may pick something
 * else, and aa_device_get_sample_rate()/aa_device_get_channel_count() report
 * what was actually granted.  frames_per_burst of 0 means "let the device
 * decide".
 *
 * Returns NULL only when out of memory; a device that cannot be opened is
 * returned as a device object with no stream and an error message.
 */
AAudioDevice* aa_device_open (int32_t sample_rate, int32_t channel_count, int32_t frames_per_burst);

/* Same, but for the default input (recording) device. */
AAudioDevice* aa_device_open_input (int32_t sample_rate, int32_t channel_count, int32_t frames_per_burst);

void    aa_device_close (AAudioDevice* device);

int     aa_device_start (AAudioDevice* device);
int     aa_device_stop  (AAudioDevice* device);

/* Pull frames from the stream.  Returns the number of frames read, or one of
 * the negative status codes. */
int32_t aa_device_read  (AAudioDevice* device, void* buffer, int32_t frames, int64_t timeout_nanos);

/* Push frames into the stream.  Returns the number of frames written, or one
 * of the negative status codes. */
int32_t aa_device_write (AAudioDevice* device, const void* buffer, int32_t frames, int64_t timeout_nanos);

int32_t aa_device_get_sample_rate     (const AAudioDevice* device);
int32_t aa_device_get_channel_count   (const AAudioDevice* device);
int32_t aa_device_get_frames_per_burst(const AAudioDevice* device);
int32_t aa_device_get_buffer_capacity (const AAudioDevice* device);
int32_t aa_device_get_format          (const AAudioDevice* device);
int32_t aa_device_get_device_id       (const AAudioDevice* device);
int32_t aa_device_get_xrun_count      (const AAudioDevice* device);

/* The last error as a human readable string (never NULL). */
const char* aa_device_get_last_error (const AAudioDevice* device);

/* Enumeration of the audio devices, as a human readable list ("id: name",
 * one per line).  AAudio only exposes this from API 30 on; on older systems
 * the list is empty and aa_device_supports_device_list() returns 0. */
int  aa_device_supports_device_list (void);
int  aa_device_get_device_list (char* buffer, int32_t buffer_size);

#ifdef __cplusplus
}
#endif

#endif /* __ardour_aaudio_device_h__ */
