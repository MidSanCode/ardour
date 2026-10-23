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
 * engine on Android.
 *
 * org.ardour.ardour.NativeEngine (Kotlin) calls
 *
 *   nativeInfo ()                         build information, no engine needed
 *   nativeDevices ()                      the AAudio devices, no engine needed
 *   nativeInit (libdir, datadir, userdir, cachedir)
 *                                         sets the engine's search paths up,
 *                                         initialises libardour and starts
 *                                         the audio engine
 *   nativeCall (method, params)           one request of the frontend protocol
 *
 * Everything returns JSON.  The protocol itself (the methods, their
 * parameters and replies) is platform independent and lives in
 * engine_protocol.cc; this file only adapts it to JNI.
 */

#include <jni.h>
#include <android/log.h>

#include <cstdlib>
#include <string>

#include "aaudio_device.h"
#include "engine_protocol.h"

#ifndef ARDOUR_ANDROID_VERSION
#define ARDOUR_ANDROID_VERSION "unknown"
#endif

#ifndef ARDOUR_TARGET_ABI
#define ARDOUR_TARGET_ABI "unknown"
#endif

#ifndef ARDOUR_ANDROID_API
#define ARDOUR_ANDROID_API 0
#endif

static std::string
jstring_to_string (JNIEnv* env, jstring s)
{
	if (!s) {
		return std::string ();
	}
	const char* c = env->GetStringUTFChars (s, 0);
	std::string r (c ? c : "");
	env->ReleaseStringUTFChars (s, c);
	return r;
}

static void
log_to_logcat (int level, const std::string& msg)
{
	int prio = ANDROID_LOG_INFO;
	switch (level) {
		case ArdourBridge::LogWarning: prio = ANDROID_LOG_WARN; break;
		case ArdourBridge::LogError:   prio = ANDROID_LOG_ERROR; break;
		default: break;
	}
	__android_log_write (prio, "ardour", msg.c_str ());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ardour_ardour_NativeEngine_nativeInfo (JNIEnv* env, jobject /* self */)
{
	std::string json = "{";
	json += "\"version\":\"" + ArdourBridge::json_escape (ARDOUR_ANDROID_VERSION) + "\",";
	json += "\"target_os\":\"android\",";
	json += "\"target_abi\":\"" + ArdourBridge::json_escape (ARDOUR_TARGET_ABI) + "\",";
	json += "\"api_level\":" + std::to_string (ARDOUR_ANDROID_API) + ",";
	json += "\"audio_api\":\"AAudio\",";
	json += std::string ("\"engine_running\":") + (ArdourBridge::engine_running () ? "true" : "false");
	json += "}";

	return env->NewStringUTF (json.c_str ());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ardour_ardour_NativeEngine_nativeDevices (JNIEnv* env, jobject /* self */)
{
	char buffer[4096];
	std::string json = "[";

	if (aa_device_get_device_list (buffer, sizeof (buffer)) > 0) {
		std::string list (buffer);
		size_t pos = 0;
		bool first = true;
		while (pos < list.size ()) {
			size_t eol = list.find ('\n', pos);
			if (eol == std::string::npos) {
				eol = list.size ();
			}
			std::string line = list.substr (pos, eol - pos);
			pos = eol + 1;
			if (line.empty ()) {
				continue;
			}
			size_t colon = line.find (':');
			std::string id = (colon == std::string::npos) ? std::string ("0") : line.substr (0, colon);
			std::string name = (colon == std::string::npos) ? line : line.substr (colon + 1);
			while (!name.empty () && name[0] == ' ') {
				name.erase (0, 1);
			}
			json += first ? "" : ",";
			json += "{\"id\":\"" + ArdourBridge::json_escape (id) + "\",\"name\":\"" + ArdourBridge::json_escape (name) + "\"}";
			first = false;
		}
	}

	json += "]";
	return env->NewStringUTF (json.c_str ());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ardour_ardour_NativeEngine_nativeInit (JNIEnv* env, jobject /* self */,
                                                jstring libdir, jstring datadir,
                                                jstring userdir, jstring cachedir)
{
	ArdourBridge::Paths paths;
	paths.module_dir = jstring_to_string (env, libdir);
	paths.data_dir   = jstring_to_string (env, datadir);
	paths.user_dir   = jstring_to_string (env, userdir);
	paths.cache_dir  = jstring_to_string (env, cachedir);

	ArdourBridge::set_log_sink (log_to_logcat);

	/* AAudio first; the dummy backend keeps the engine usable on an
	 * emulator or device without a usable audio output.
	 */
	std::string reply = ArdourBridge::init (paths, "AAudio", "None (Dummy)");
	return env->NewStringUTF (reply.c_str ());
}

extern "C" JNIEXPORT jstring JNICALL
Java_org_ardour_ardour_NativeEngine_nativeCall (JNIEnv* env, jobject /* self */,
                                                jstring method, jstring params)
{
	std::string reply = ArdourBridge::call (jstring_to_string (env, method), jstring_to_string (env, params));
	return env->NewStringUTF (reply.c_str ());
}
