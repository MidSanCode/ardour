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

/*
 * The native half of the bridge between the JavaScript frontend and the
 * engine.
 *
 * The functions are called from org.ardour.ardour.NativeEngine (Kotlin) and
 * return JSON, so that the frontend can parse them with JSON.parse() and be
 * evolved without a native rebuild.
 *
 * This first version reports what can be reported without the engine being
 * running: the build of the native side, and the audio devices the AAudio
 * backend can see.  The engine state (session, transport) needs the control
 * protocol to be separated from the websocket server, which is the next
 * piece of work (see _portability/plan.md).
 */

#include <jni.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "aaudio_device.h"

#ifndef ARDOUR_ANDROID_VERSION
#define ARDOUR_ANDROID_VERSION "unknown"
#endif

#ifndef ARDOUR_TARGET_OS
#define ARDOUR_TARGET_OS "unknown"
#endif

#ifndef ARDOUR_TARGET_ABI
#define ARDOUR_TARGET_ABI "unknown"
#endif

#ifndef ARDOUR_ANDROID_API
#define ARDOUR_ANDROID_API 0
#endif

static std::string
json_escape (const std::string& in)
{
	std::string out;

	for (char c : in) {
		switch (c) {
		case '"':  out += "\\\""; break;
		case '\\': out += "\\\\"; break;
		case '\n': out += "\\n"; break;
		case '\r': out += "\\r"; break;
		case '\t': out += "\\t"; break;
		default:
			if ((unsigned char) c < 0x20) {
				char tmp[8];
				snprintf (tmp, sizeof (tmp), "\\u%04x", (unsigned char) c);
				out += tmp;
			} else {
				out += c;
			}
		}
	}

	return out;
}

/* the device list of the AAudio API, as "id: name" lines */
static std::vector<std::string>
device_lines ()
{
	std::vector<std::string> lines;
	char buffer[4096];

	if (aa_device_get_device_list (buffer, sizeof (buffer)) <= 0) {
		return lines;
	}

	std::string list (buffer);
	size_t pos = 0;

	while (pos < list.size ()) {
		size_t eol = list.find ('\n', pos);
		if (eol == std::string::npos) {
			eol = list.size ();
		}
		std::string line = list.substr (pos, eol - pos);
		pos = eol + 1;
		if (!line.empty ()) {
			lines.push_back (line);
		}
	}

	return lines;
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ardour_ardour_NativeEngine_nativeInfo (JNIEnv* env, jobject /* self */)
{
	std::string json = "{";
	json += "\"version\":\"" + json_escape (ARDOUR_ANDROID_VERSION) + "\",";
	json += "\"target_os\":\"" + json_escape (ARDOUR_TARGET_OS) + "\",";
	json += "\"target_abi\":\"" + json_escape (ARDOUR_TARGET_ABI) + "\",";
	json += "\"api_level\":" + std::to_string (ARDOUR_ANDROID_API) + ",";
	json += "\"audio_api\":\"AAudio\",";
	json += "\"engine_running\":false";
	json += "}";

	return env->NewStringUTF (json.c_str ());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ardour_ardour_NativeEngine_nativeDevices (JNIEnv* env, jobject /* self */)
{
	std::string json = "[";

	std::vector<std::string> lines = device_lines ();

	for (size_t n = 0; n < lines.size (); ++n) {

		const std::string& line = lines[n];
		size_t colon = line.find (':');

		std::string id = (colon == std::string::npos) ? std::string ("0") : line.substr (0, colon);
		std::string name = (colon == std::string::npos) ? line : line.substr (colon + 1);

		while (!name.empty () && name[0] == ' ') {
			name.erase (0, 1);
		}

		if (n > 0) {
			json += ",";
		}

		json += "{\"id\":\"" + json_escape (id) + "\",";
		json += "\"name\":\"" + json_escape (name) + "\"}";
	}

	json += "]";

	return env->NewStringUTF (json.c_str ());
}
