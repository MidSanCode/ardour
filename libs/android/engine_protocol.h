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
 *   engine.buffer                         the block size and the sizes on offer
 *   engine.buffer    {samples}            changes it: the engine is stopped,
 *                                         resized and started again, and the old
 *                                         size is put back if the device refuses.
 *                                         AAudio cannot resize an open stream, so
 *                                         this is the only way to change it.
 *   session.list                          the sessions in <user_dir>/sessions, each with
 *                                         its size on disk, when it was last written
 *                                         and whether it is the one loaded now
 *   session.create   {name, template?}    creates and loads a new session; the
 *                                         template is a name from session.templates
 *   session.open     {name}               loads an existing session
 *   session.save
 *   session.close
 *   session.info                          the loaded session: name, path, rate,
 *                                         tracks, buses, size, unsaved changes
 *   session.rename   {name}               renames the loaded session's directory,
 *                                         state file, history and interchange folder
 *   session.delete   {name, confirm}      removes a session and its audio; the name
 *                                         has to be repeated in "confirm", and the
 *                                         loaded session cannot be removed
 *   session.save_as  {name, copy_media?}  writes a copy; without copy_media it
 *                                         points at this session's audio instead
 *   session.templates                     the session templates on the device
 *   session.save_template   {name, description?}
 *   session.delete_template {name}        only the user's own templates
 *   transport.play / transport.stop
 *   transport.state                       rolling, record_enabled, recording, position
 *   transport.record {enabled}            arms or disarms the session for recording
 *   tracks.list                           every track: id, name, channels
 *   tracks.add       {name, channels}     adds an audio track (1 or 2 channels)
 *   tracks.remove    {id}                 removes a track or bus
 *   meters                                peak level in dBFS per strip and channel
 *
 *   mixer.strips                          tracks, busses and the master bus: gain_db,
 *                                         pan (-1 .. 1 or null), mute, solo, rec,
 *                                         plugins, and sends (target, name, gain_db,
 *                                         pre, active)
 *   strip.set        {id, gain_db?, pan?, mute?, solo?, rec?, name?}
 *   buses.add        {name}               adds a stereo audio bus
 *
 *   send.add         {track, target, pre?, gain_db?}
 *                                         feeds track's output into target's input;
 *                                         pre is pre-fader (default post-fader).
 *                                         The target gains an internal return, so it
 *                                         starts showing up as a bus.
 *   send.set         {track, target, gain_db?, pre?, active?}
 *                                         pre moves the send to the other side of the
 *                                         fader (it is a chain position, so it is
 *                                         re-inserted and keeps its gain)
 *   send.remove      {track, target}
 *                                         all three reply with the updated strip
 *
 *   timeline                              every track with its regions (samples)
 *   regions.tone     {track, position?, seconds?, frequency?}
 *                                         records a sine tone into a new region
 *   regions.import   {track, path, position?}
 *                                         copies a sound file into the session
 *   region.move      {id, position?, track?}
 *   region.trim      {id, start?, end?}   new edges on the timeline (samples)
 *   region.fades     {id, fade_in?, fade_out?}
 *   region.split     {id, position}
 *   region.mute      {id, muted}
 *   region.remove    {id}
 *   region.peaks     {id, count}          min/max pairs of the first channel
 *   transport.locate {position}
 *   tracks.add {kind: "midi", name?, instrument?}
 *                                         a MIDI track; instrument is a plugin id, "none",
 *                                         or left out for ACE Reasonable Synth
 *   regions.midi     {track, position?, beats?}   an empty MIDI region
 *   region.notes     {id}                 notes: id, note, velocity, channel, start, length
 *                                         (beats from the start of the region's source;
 *                                         the region reports start_beats, length_beats)
 *   region.note.add  {id, note, start, length?, velocity?, channel?}
 *   region.note.edit {id, note_id, note?, start?, length?, velocity?}
 *   region.note.remove {id, note_id}
 *   plugins.list     {instruments?}       id, name, type, category, creator, instrument
 *   plugin.add       {route, id, post?}   inserts a plugin; replies with its parameters
 *   plugin.get       {route, plugin}
 *   plugin.set       {route, plugin, active?, param?, value?}
 *   plugin.remove    {route, plugin}
 *                                         mixer.strips lists each strip's plugins
 *   midi.send        {track?, data: "90 3c 64 ..."} or {track?, note, velocity?, on?, channel?}
 *                                         MIDI into the engine's input, which MIDI tracks
 *                                         hear (and record when armed)
 *   plugin.presets   {route, plugin}      presets: uri, label, user; current
 *   plugin.preset.load {route, plugin, uri}
 *   plugin.preset.save {route, plugin, name}
 *   automation.get   {route, control?, plugin?, param?}
 *                                         control: gain (default), trim, pan, mute, plugin;
 *                                         state, min, max, points [{t, value, y}]
 *   automation.state {.., state}          manual, play, write, touch, latch
 *   automation.add   {.., t, value | y}   (y is 0..1 as a fader shows it; undoable)
 *   automation.remove {.., index}   automation.clear {..}
 *   regions.import   now resamples a file at another rate (reply: resampled_from)
 *   edit.undo / edit.redo                 region edits are undoable; timeline and these
 *                                         replies carry the next undo/redo names
 *
 *   export.presets                        the export presets found on this device:
 *                                         id, name; and the one used by default
 *   export.audio     {preset?, start?, end?, folder?, name?}
 *                                         renders the master bus through libardour's
 *                                         freewheel export and WAITS for it; without a
 *                                         range it exports from the start to the last
 *                                         region's end, and without a folder it writes
 *                                         into <cache>/export.  Replies with folder,
 *                                         seconds and the files that appeared.
 *   export.start     {preset?, start?, end?, folder?, name?}
 *                                         the same, but returns at once: the render is
 *                                         watched on a thread of its own, so the engine
 *                                         thread (the one the UI talks to) stays free
 *   export.status                         running, finished, succeeded, cancelled,
 *                                         progress 0..1, seconds, folder, files
 *   export.cancel                         aborts the running export; it still has to
 *                                         be polled until finished, which is when the
 *                                         transport leaves freewheel
 *
 * All engine work happens on one thread owned by this module, which carries
 * the event loop and the session event pool libardour expects of a "GUI"
 * thread; call() hands a request to it and waits for the reply.
 *
 * shutdown() is not a protocol method: it is called by the host when the
 * application is going away, and there is nothing left to talk to afterwards.
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

/* Stops the session, the engine and libardour, in that order, so that a
 * process that is about to exit leaves its files and the audio device in a
 * sane state.  Idempotent, and init() refuses to run after it.
 */
std::string shutdown ();

std::string json_escape (std::string const&);

}

#endif
