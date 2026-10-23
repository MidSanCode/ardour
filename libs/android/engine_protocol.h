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
*/

/*
 * The engine side of the frontend protocol: a small JSON request/reply API
 * over libardour, shared by every embedding (JNI on Android, and later the
 * iOS and wasm hosts).
 *
 * A request is a method name and a flat JSON object of parameters; the reply
 * is a JSON object, {"ok":true, ...} or {"ok":false,"error":"..."}.
 *
 *   engine.status                         engine and session state
 *   session.list                          the sessions in <user_dir>/sessions
 *   session.create   {name}               creates and loads a new session
 *   session.open     {name}               loads an existing session
 *   session.save
 *   session.close
 *   transport.play / transport.stop
 *   transport.state                       rolling, position (samples, seconds)
 *   tracks.list                           every track: id, name, channels
 *   tracks.add       {name, channels}     adds an audio track (1 or 2 channels)
 *   tracks.remove    {id}                 removes a track or bus
 *   meters                                peak level in dBFS per strip and channel
 *
 *   mixer.strips                          tracks, busses and the master bus: gain_db,
 *                                         pan (-1 .. 1 or null), mute, solo, rec
 *   strip.set        {id, gain_db?, pan?, mute?, solo?, rec?, name?}
 *   buses.add        {name}               adds a stereo audio bus
 *
 *   timeline                              every track with its regions (samples)
 *   regions.tone     {track, position?, seconds?, frequency?}
 *                                         records a sine tone into a new region
 *   region.move      {id, position?, track?}
 *   region.split     {id, position}
 *   region.mute      {id, muted}
 *   region.remove    {id}
 *   region.peaks     {id, count}          min/max pairs of the first channel
 *   transport.locate {position}
 *
 * All engine work happens on one thread owned by this module, which carries
 * the event loop and the session event pool libardour expects of a "GUI"
 * thread; call() hands a request to it and waits for the reply.
 */

#ifndef __ardour_android_engine_protocol_h__
#define __ardour_android_engine_protocol_h__

#include <string>

namespace ArdourBridge {

struct Paths {
	std::string module_dir; /* backends and panners (Android: nativeLibraryDir) */
	std::string data_dir;   /* the engine's data files (share/ardourN) */
	std::string user_dir;   /* configuration and sessions */
	std::string cache_dir;
};

enum LogLevel { LogInfo, LogWarning, LogError };
typedef void (*LogSink) (int level, const std::string&);

void        set_log_sink (LogSink);
std::string init (Paths const&, std::string const& backend, std::string const& fallback_backend);
std::string call (std::string const& method, std::string const& params);
bool        engine_running ();

std::string json_escape (std::string const&);

}

#endif
