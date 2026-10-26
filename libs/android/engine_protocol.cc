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

#include <pthread.h>

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <deque>
#include <exception>
#include <functional>
#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <cstring>

#include <sndfile.h>
#include <samplerate.h>

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "pbd/basename.h"
#include "pbd/error.h"
#include "pbd/memento_command.h"
#include "pbd/event_loop.h"
#include "pbd/failed_constructor.h"
#include "pbd/receiver.h"
#include "pbd/rwlock.h"
#include "pbd/transmitter.h"

#include "ardour/ardour.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/audiofilesource.h"
#include "ardour/audioregion.h"
#include "ardour/dB.h"
#include "ardour/file_source.h"
#include "ardour/filename_extensions.h"
#include "ardour/meter.h"
#include "ardour/async_midi_port.h"
#include "ardour/automation_control.h"
#include "ardour/automation_list.h"
#include "ardour/io.h"
#include "ardour/midi_port.h"
#include "ardour/midi_model.h"
#include "ardour/midi_region.h"
#include "ardour/midi_source.h"
#include "ardour/midi_track.h"
#include "ardour/plugin.h"
#include "ardour/plugin_insert.h"
#include "ardour/plugin_manager.h"
#include "ardour/playlist.h"
#include "ardour/presentation_info.h"
#include "ardour/region_factory.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/session_event.h"
#include "ardour/track.h"
#include "ardour/types.h"
#include "ardour/utils.h"

#include "engine_protocol.h"

using namespace ARDOUR;

namespace ArdourBridge {

/* ---------------------------------------------------------------------------
 * JSON helpers
 */

std::string
json_escape (std::string const& in)
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

static std::string
q (std::string const& s)
{
	return "\"" + json_escape (s) + "\"";
}

static std::string
num (double v)
{
	if (!std::isfinite (v)) {
		/* JSON has no infinity: silence is reported as -200 dBFS */
		v = -200;
	}
	char buf[32];
	snprintf (buf, sizeof (buf), "%.3f", v);
	return buf;
}

static std::string
error_reply (std::string const& msg)
{
	return "{\"ok\":false,\"error\":" + q (msg) + "}";
}

/* The parameters of a request: a flat JSON object whose values are strings,
 * numbers or booleans.  Values are kept as text.
 */
typedef std::map<std::string, std::string> Params;

static void
skip_ws (std::string const& s, size_t& i)
{
	while (i < s.size () && (s[i] == ' ' || s[i] == '\t' || s[i] == '\n' || s[i] == '\r')) {
		++i;
	}
}

static bool
parse_string (std::string const& s, size_t& i, std::string& out)
{
	if (i >= s.size () || s[i] != '"') {
		return false;
	}
	++i;
	while (i < s.size () && s[i] != '"') {
		if (s[i] == '\\' && i + 1 < s.size ()) {
			++i;
			switch (s[i]) {
				case 'n': out += '\n'; break;
				case 't': out += '\t'; break;
				case 'r': out += '\r'; break;
				case 'u':
					/* only the ASCII range is needed for names here */
					if (i + 4 < s.size ()) {
						out += (char) strtol (s.substr (i + 1, 4).c_str (), 0, 16);
						i += 4;
					}
					break;
				default: out += s[i]; break;
			}
		} else {
			out += s[i];
		}
		++i;
	}
	if (i >= s.size ()) {
		return false;
	}
	++i;
	return true;
}

static bool
parse_params (std::string const& s, Params& p)
{
	size_t i = 0;
	skip_ws (s, i);
	if (i == s.size ()) {
		return true;
	}
	if (s[i] != '{') {
		return false;
	}
	++i;
	while (true) {
		skip_ws (s, i);
		if (i < s.size () && s[i] == '}') {
			return true;
		}
		std::string key, value;
		if (!parse_string (s, i, key)) {
			return false;
		}
		skip_ws (s, i);
		if (i >= s.size () || s[i] != ':') {
			return false;
		}
		++i;
		skip_ws (s, i);
		if (i < s.size () && s[i] == '"') {
			if (!parse_string (s, i, value)) {
				return false;
			}
		} else {
			while (i < s.size () && s[i] != ',' && s[i] != '}') {
				value += s[i++];
			}
			while (!value.empty () && (value.back () == ' ' || value.back () == '\n')) {
				value.pop_back ();
			}
		}
		p[key] = value;
		skip_ws (s, i);
		if (i < s.size () && s[i] == ',') {
			++i;
			continue;
		}
		if (i < s.size () && s[i] == '}') {
			return true;
		}
		return false;
	}
}

/* ---------------------------------------------------------------------------
 * logging
 */

static LogSink log_sink = 0;

void
set_log_sink (LogSink s)
{
	log_sink = s;
}

static void
log (int level, std::string const& msg)
{
	if (log_sink) {
		log_sink (level, msg);
	} else {
		fprintf (stderr, "ardour: %s\n", msg.c_str ());
	}
}

class LogReceiver : public Receiver
{
protected:
	void receive (Transmitter::Channel chn, const char* str)
	{
		switch (chn) {
			case Transmitter::Debug:
				return;
			case Transmitter::Info:
				log (LogInfo, str);
				break;
			case Transmitter::Warning:
				log (LogWarning, str);
				break;
			case Transmitter::Error:
			case Transmitter::Fatal:
			case Transmitter::Throw:
				log (LogError, str);
				break;
		}
	}
};

static LogReceiver log_receiver;

/* ---------------------------------------------------------------------------
 * the engine thread
 */

/* libardour expects the thread that drives a session (normally the GUI
 * thread) to have an event loop; requests from other threads are not needed
 * here, so a slot is only ever run when it is called from the thread itself.
 */
class BridgeEventLoop : public PBD::EventLoop
{
public:
	BridgeEventLoop () : PBD::EventLoop ("bridge"), _thread (pthread_self ()) {}

	/* A slot from another thread is refused rather than dropped, so that the
	 * caller can fall back: the process thread then hands the clean-up of a
	 * realtime session event to the butler instead of leaking the event.
	 */
	bool call_slot (PBD::EventLoop::InvalidationRecord*, const std::function<void()>& f)
	{
		if (pthread_equal (pthread_self (), _thread)) {
			f ();
			return true;
		}
		return false;
	}

	PBD::RWLock& slot_invalidation_rwlock () { return _lock; }

private:
	pthread_t   _thread;
	PBD::RWLock _lock;
};

typedef std::function<std::string ()> Job;

struct Request {
	Job         job;
	std::string reply;
	bool        done;
	Request (Job const& j) : job (j), done (false) {}
};

static std::mutex                            queue_lock;
static std::condition_variable               queue_cond;
static std::deque<std::shared_ptr<Request> > queue;
static bool                                  thread_started = false;
static pthread_t                             engine_thread;

static void*
engine_thread_main (void*)
{
	while (true) {
		std::shared_ptr<Request> r;
		{
			std::unique_lock<std::mutex> lm (queue_lock);
			queue_cond.wait (lm, [] { return !queue.empty (); });
			r = queue.front ();
			queue.pop_front ();
		}

		std::string reply;
		try {
			reply = r->job ();
		} catch (failed_constructor const&) {
			reply = error_reply ("the engine could not construct the object");
		} catch (AudioEngine::PortRegistrationFailure const& e) {
			reply = error_reply (std::string ("port registration failed: ") + e.what ());
		} catch (std::exception const& e) {
			reply = error_reply (std::string ("exception: ") + e.what ());
		} catch (...) {
			reply = error_reply ("unknown exception");
		}

		{
			std::unique_lock<std::mutex> lm (queue_lock);
			r->reply = reply;
			r->done  = true;
		}
		queue_cond.notify_all ();
	}
	return 0;
}

static std::string
run_on_engine_thread (Job const& job)
{
	std::shared_ptr<Request> r (new Request (job));
	std::unique_lock<std::mutex> lm (queue_lock);

	if (!thread_started) {
		pthread_attr_t attr;
		pthread_attr_init (&attr);
		/* loading a session recurses deeply (XML state, Lua) */
		pthread_attr_setstacksize (&attr, 8 * 1024 * 1024);
		if (pthread_create (&engine_thread, &attr, engine_thread_main, 0)) {
			pthread_attr_destroy (&attr);
			return error_reply ("cannot create the engine thread");
		}
		pthread_attr_destroy (&attr);
		thread_started = true;
	}

	queue.push_back (r);
	queue_cond.notify_all ();
	queue_cond.wait (lm, [&r] { return r->done; });
	return r->reply;
}

/* ---------------------------------------------------------------------------
 * state, only touched on the engine thread
 */

static bool             initialized = false;
static BridgeEventLoop* event_loop = 0;
static Session*         session = 0;
static std::string      sessions_root;
static std::string      backend_names[2];

bool
engine_running ()
{
	return initialized && AudioEngine::instance () && AudioEngine::instance ()->running ();
}

static void
set_env (const char* name, std::string const& value)
{
	if (!value.empty ()) {
		setenv (name, value.c_str (), 1);
	}
}

static bool
start_backend (std::string const& name, std::string& why)
{
	AudioEngine* engine = AudioEngine::instance ();

	if (!engine->set_backend (name, PROGRAM_NAME, "")) {
		why = "backend '" + name + "' is not available";
		return false;
	}
	if (engine->start () != 0) {
		why = "backend '" + name + "' did not start";
		return false;
	}
	return true;
}

static std::string
status_json ()
{
	AudioEngine* engine = AudioEngine::instance ();
	std::string j = "{\"ok\":true";
	j += std::string (",\"initialized\":") + (initialized ? "true" : "false");
	j += std::string (",\"running\":") + (engine_running () ? "true" : "false");
	if (engine) {
		j += ",\"backend\":" + q (engine->current_backend_name ());
		if (engine->running ()) {
			j += ",\"sample_rate\":" + std::to_string ((long long) engine->sample_rate ());
			j += ",\"buffer_size\":" + std::to_string ((long long) engine->samples_per_cycle ());
			j += ",\"dsp_load\":" + num (engine->get_dsp_load ());
		}
	}
	if (session) {
		j += ",\"session\":{\"name\":" + q (session->name ()) + ",\"path\":" + q (session->path ()) + "}";
	} else {
		j += ",\"session\":null";
	}
	j += "}";
	return j;
}

static std::string
do_init (Paths const& paths)
{
	if (initialized) {
		return status_json ();
	}

	/* where libardour looks for its modules and data, see search_paths.cc
	 * and filesystem_paths.cc
	 */
	set_env ("ARDOUR_BACKEND_PATH", paths.module_dir);
	set_env ("ARDOUR_PANNER_PATH", paths.module_dir);
	set_env ("ARDOUR_DLL_PATH", paths.module_dir);
	set_env ("ARDOUR_DATA_PATH", paths.data_dir);
	set_env ("ARDOUR_CONFIG_PATH", paths.data_dir);
	/* the bundled LV2 plugins: their descriptions are with the data files,
	 * their code with the other native libraries (see collect-engine.sh) */
	set_env ("LV2_PATH", Glib::build_filename (paths.data_dir, "LV2"));
	set_env ("HOME", paths.user_dir);
	set_env ("XDG_CONFIG_HOME", Glib::build_filename (paths.user_dir, "config"));
	set_env ("XDG_CACHE_HOME", paths.cache_dir);
	set_env ("TMPDIR", paths.cache_dir);

	sessions_root = Glib::build_filename (paths.user_dir, "sessions");
	g_mkdir_with_parents (sessions_root.c_str (), 0755);

	log_receiver.listen_to (PBD::info);
	log_receiver.listen_to (PBD::warning);
	log_receiver.listen_to (PBD::error);
	log_receiver.listen_to (PBD::fatal);

	if (!ARDOUR::init (true, "")) {
		return error_reply ("libardour failed to initialize");
	}

	event_loop = new BridgeEventLoop ();
	PBD::EventLoop::set_event_loop_for_thread (event_loop);
	SessionEvent::create_per_thread_pool ("bridge", 4096);

	AudioEngine::create ();

	std::string why_first, why_second;
	if (!start_backend (backend_names[0], why_first)) {
		log (LogWarning, why_first + ", falling back to '" + backend_names[1] + "'");
		if (backend_names[1].empty () || !start_backend (backend_names[1], why_second)) {
			return error_reply (why_first + (why_second.empty () ? "" : "; " + why_second));
		}
	}

	initialized = true;
	return status_json ();
}

/* ---------------------------------------------------------------------------
 * the methods
 */

static std::string
require_session ()
{
	return session ? std::string () : error_reply ("no session is loaded");
}

static bool
valid_session_name (std::string const& name)
{
	if (name.empty () || name[0] == '.') {
		return false;
	}
	return name.find_first_of ("/\\:") == std::string::npos;
}

static void
close_session ()
{
	if (!session) {
		return;
	}
	session->request_stop ();
	delete session;
	session = 0;
}

static std::string
m_session_list ()
{
	std::string j = "{\"ok\":true,\"sessions\":[";
	bool first = true;
	try {
		Glib::Dir dir (sessions_root);
		for (Glib::Dir::iterator i = dir.begin (); i != dir.end (); ++i) {
			std::string name = *i;
			std::string state = Glib::build_filename (sessions_root, name, name + statefile_suffix);
			if (Glib::file_test (state, Glib::FILE_TEST_IS_REGULAR)) {
				j += (first ? "" : ",") + q (name);
				first = false;
			}
		}
	} catch (...) {
	}
	j += "]}";
	return j;
}

static std::string
m_session_create (Params& p)
{
	std::string name = p["name"];
	if (!valid_session_name (name)) {
		return error_reply ("invalid session name");
	}
	std::string dir = Glib::build_filename (sessions_root, name);
	if (Glib::file_test (Glib::build_filename (dir, name + statefile_suffix), Glib::FILE_TEST_EXISTS)) {
		return error_reply ("a session with that name exists");
	}
	if (!engine_running ()) {
		return error_reply ("the engine is not running");
	}

	close_session ();

	BusProfile bus_profile;
	bus_profile.master_out_channels = 2;
	session = new Session (*AudioEngine::instance (), dir, name, &bus_profile);
	AudioEngine::instance ()->set_session (session);
	session->save_state ("");
	return status_json ();
}

static std::string
m_session_open (Params& p)
{
	std::string name = p["name"];
	if (!valid_session_name (name)) {
		return error_reply ("invalid session name");
	}
	std::string dir = Glib::build_filename (sessions_root, name);
	if (!Glib::file_test (Glib::build_filename (dir, name + statefile_suffix), Glib::FILE_TEST_IS_REGULAR)) {
		return error_reply ("no such session");
	}
	if (!engine_running ()) {
		return error_reply ("the engine is not running");
	}

	close_session ();

	session = new Session (*AudioEngine::instance (), dir, name);
	AudioEngine::instance ()->set_session (session);
	return status_json ();
}

static std::string
m_session_save ()
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	if (session->save_state ("") != 0) {
		return error_reply ("saving the session failed");
	}
	return "{\"ok\":true}";
}

static std::string
transport_json ()
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	samplepos_t pos = session->transport_sample ();
	double sr = session->sample_rate ();
	std::string j = "{\"ok\":true";
	j += std::string (",\"rolling\":") + (session->transport_rolling () ? "true" : "false");
	j += std::string (",\"record_enabled\":") + (session->record_status () >= ARDOUR::Enabled ? "true" : "false");
	j += std::string (",\"recording\":") + (session->actively_recording () ? "true" : "false");
	j += ",\"position\":" + std::to_string ((long long) pos);
	j += ",\"seconds\":" + num (sr > 0 ? pos / sr : 0);
	j += "}";
	return j;
}

static std::string
m_transport (bool roll)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	if (roll) {
		session->request_roll (TRS_UI);
	} else {
		session->request_stop ();
	}
	return transport_json ();
}

static std::string
m_tracks_list ()
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::string j = "{\"ok\":true,\"tracks\":[";
	bool first = true;
	std::shared_ptr<RouteList> tracks = session->get_tracks ();
	for (auto const& r : *tracks) {
		j += first ? "" : ",";
		j += "{\"id\":" + q (r->id ().to_s ());
		j += ",\"name\":" + q (r->name ());
		j += ",\"inputs\":" + std::to_string (r->n_inputs ().n_audio ());
		j += ",\"outputs\":" + std::to_string (r->n_outputs ().n_audio ());
		j += std::string (",\"audio\":") + (std::dynamic_pointer_cast<AudioTrack> (r) ? "true" : "false");
		j += "}";
		first = false;
	}
	j += "]}";
	return j;
}

static PluginInfoPtr find_plugin_info (std::string const& id);

/* MIDI from the app (the on-screen keyboard, MIDI devices on Android) goes
 * through the engine's virtual keyboard port, which MIDI tracks listen to */
static void
connect_midi_input (std::shared_ptr<Track> const& t)
{
	std::shared_ptr<AsyncMIDIPort> vk = session ? session->vkbd_output_port () : std::shared_ptr<AsyncMIDIPort> ();
	if (!vk || !t || t->input ()->n_ports ().n_midi () == 0) {
		return;
	}
	std::shared_ptr<MidiPort> in = t->input ()->midi (0);
	std::string const vk_name = std::static_pointer_cast<ARDOUR::Port> (vk)->name ();
	if (in && !in->connected_to (vk_name)) {
		in->connect (vk_name);
	}
}
static PluginInfoPtr default_instrument ();

static std::string
m_tracks_add (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	if (p["kind"] == "midi") {
		std::string   name = p["name"].empty () ? std::string ("MIDI") : p["name"];
		PluginInfoPtr instrument;
		if (p["instrument"].empty ()) {
			instrument = default_instrument ();
		} else if (p["instrument"] != "none" && p["instrument"] != "null") {
			instrument = find_plugin_info (p["instrument"]);
			if (!instrument) {
				return error_reply ("no such plugin: " + p["instrument"]);
			}
		}
		std::list<std::shared_ptr<MidiTrack> > added =
			session->new_midi_track (ChanCount (DataType::MIDI, 1), ChanCount (DataType::MIDI, 1), false,
			                         instrument, 0, std::shared_ptr<RouteGroup> (), 1, name,
			                         PresentationInfo::max_order, Normal, true);
		if (added.empty ()) {
			return error_reply ("the track could not be added");
		}
		connect_midi_input (added.front ());
		return "{\"ok\":true,\"id\":" + q (added.front ()->id ().to_s ()) + ",\"name\":" + q (added.front ()->name ()) + "}";
	}
	std::string name = p["name"].empty () ? std::string ("Audio") : p["name"];
	int channels = p["channels"].empty () ? 1 : atoi (p["channels"].c_str ());
	if (channels < 1 || channels > 2) {
		return error_reply ("channels must be 1 or 2");
	}
	std::list<std::shared_ptr<AudioTrack> > added =
		session->new_audio_track (channels, 2, std::shared_ptr<RouteGroup> (), 1, name,
		                          PresentationInfo::max_order, Normal, true);
	if (added.empty ()) {
		return error_reply ("the track could not be added");
	}
	return "{\"ok\":true,\"id\":" + q (added.front ()->id ().to_s ()) + ",\"name\":" + q (added.front ()->name ()) + "}";
}

/* ---------------------------------------------------------------------------
 * mixer
 */

static bool
truthy (std::string const& v)
{
	return v == "true" || v == "1";
}

static bool
by_order (std::shared_ptr<Route> const& a, std::shared_ptr<Route> const& b)
{
	return a->presentation_info ().order () < b->presentation_info ().order ();
}

/* the routes a mixer shows: tracks and busses in presentation order, then
 * the master bus
 */
static std::vector<std::shared_ptr<Route> >
mixer_routes ()
{
	std::vector<std::shared_ptr<Route> > out;
	std::shared_ptr<Route>               master;
	std::shared_ptr<RouteList const>     routes = session->get_routes ();
	for (auto const& r : *routes) {
		if (r->is_master ()) {
			master = r;
		} else if (!r->is_monitor () && !r->is_auditioner () && !r->is_surround_master ()) {
			out.push_back (r);
		}
	}
	std::sort (out.begin (), out.end (), by_order);
	if (master) {
		out.push_back (master);
	}
	return out;
}

static std::vector<std::shared_ptr<Track> >
tracks_in_order ()
{
	std::vector<std::shared_ptr<Track> > out;
	for (auto const& r : mixer_routes ()) {
		std::shared_ptr<Track> t = std::dynamic_pointer_cast<Track> (r);
		if (t) {
			out.push_back (t);
		}
	}
	return out;
}

static std::shared_ptr<Route>
find_route (std::string const& id)
{
	if (id.empty ()) {
		return std::shared_ptr<Route> ();
	}
	return session->route_by_id (PBD::ID (id));
}

static std::string
strip_kind (std::shared_ptr<Route> const& r)
{
	if (r->is_master ()) {
		return "master";
	}
	if (std::dynamic_pointer_cast<AudioTrack> (r)) {
		return "audio";
	}
	if (std::dynamic_pointer_cast<Track> (r)) {
		return "midi";
	}
	return "bus";
}

/* ---------------------------------------------------------------------------
 * plugins
 */

static const char*
plugin_type_name (PluginType t)
{
	switch (t) {
		case LV2:
			return "lv2";
		case LADSPA:
			return "ladspa";
		case Lua:
			return "lua";
		default:
			return "other";
	}
}

/* every plugin the engine can load, scanned the first time it is asked for */
static PluginInfoList
all_plugins ()
{
	static bool scanned = false;
	PluginManager& pm = PluginManager::instance ();
	if (!scanned) {
		scanned = true;
		pm.refresh (true);
	}
	PluginInfoList out;
	for (auto const& l : { &pm.lv2_plugin_info (), &pm.lua_plugin_info (), &pm.ladspa_plugin_info () }) {
		out.insert (out.end (), l->begin (), l->end ());
	}
	return out;
}

static PluginInfoPtr
find_plugin_info (std::string const& id)
{
	for (auto const& i : all_plugins ()) {
		if (i->unique_id == id) {
			return i;
		}
	}
	return PluginInfoPtr ();
}

/* the default instrument for new MIDI tracks: ACE Reasonable Synth, else the
 * first instrument there is */
static PluginInfoPtr
default_instrument ()
{
	PluginInfoPtr first;
	for (auto const& i : all_plugins ()) {
		if (!i->is_instrument ()) {
			continue;
		}
		if (i->name.find ("Reasonable") != std::string::npos) {
			return i;
		}
		if (!first) {
			first = i;
		}
	}
	return first;
}

static std::vector<std::shared_ptr<PluginInsert> >
route_plugins (std::shared_ptr<Route> const& r)
{
	std::vector<std::shared_ptr<PluginInsert> > out;
	for (uint32_t n = 0;; ++n) {
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (r->nth_plugin (n));
		if (!pi) {
			break;
		}
		out.push_back (pi);
	}
	return out;
}

static std::string
plugin_brief_json (std::shared_ptr<PluginInsert> const& pi)
{
	std::string j = "{\"id\":" + q (pi->id ().to_s ());
	j += ",\"name\":" + q (pi->name ());
	j += std::string (",\"active\":") + (pi->enabled () ? "true" : "false");
	j += std::string (",\"instrument\":") + (pi->plugin ()->get_info ()->is_instrument () ? "true" : "false");
	j += "}";
	return j;
}

static std::string
strip_json (std::shared_ptr<Route> const& r)
{
	std::string j = "{\"id\":" + q (r->id ().to_s ());
	j += ",\"name\":" + q (r->name ());
	j += ",\"kind\":" + q (strip_kind (r));
	j += ",\"channels\":" + std::to_string (std::max<uint32_t> (1, r->n_outputs ().n_audio ()));

	std::shared_ptr<AutomationControl> gain = r->gain_control ();
	j += ",\"gain_db\":" + num (gain ? accurate_coefficient_to_dB (gain->get_value ()) : 0);

	std::shared_ptr<AutomationControl> pan = r->pan_azimuth_control ();
	/* azimuth 0 is hard left and 1 hard right; the protocol uses -1 .. 1 */
	j += ",\"pan\":" + (pan ? num (pan->get_value () * 2.0 - 1.0) : std::string ("null"));

	std::shared_ptr<AutomationControl> mute = r->mute_control ();
	j += std::string (",\"mute\":") + (mute && mute->get_value () > 0.5 ? "true" : "false");
	std::shared_ptr<AutomationControl> solo = r->solo_control ();
	j += std::string (",\"solo\":") + (solo && solo->get_value () > 0.5 ? "true" : "false");

	std::shared_ptr<AutomationControl> rec = r->rec_enable_control ();
	j += ",\"rec\":" + (rec ? std::string (rec->get_value () > 0.5 ? "true" : "false") : std::string ("null"));
	j += ",\"plugins\":[";
	bool first = true;
	for (auto const& pi : route_plugins (r)) {
		j += (first ? "" : ",") + plugin_brief_json (pi);
		first = false;
	}
	j += "]}";
	return j;
}

static std::string
m_mixer_strips ()
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::string j = "{\"ok\":true,\"strips\":[";
	bool first = true;
	for (auto const& r : mixer_routes ()) {
		j += (first ? "" : ",") + strip_json (r);
		first = false;
	}
	j += "]}";
	return j;
}

/* Mute, solo and record-arm are applied by the process thread through the
 * session's realtime event queue, which waits while the butler finishes
 * non-realtime work (after adding a route, say); wait for the change to land,
 * up to two seconds, so the reply describes the new state.
 */
static void
set_control (std::shared_ptr<AutomationControl> c, double v)
{
	if (!c) {
		return;
	}
	c->set_value (v, PBD::Controllable::NoGroup);
	for (int i = 0; fabs (c->get_value () - v) > 1e-4 * std::max (1.0, fabs (v)); ++i) {
		if (i == 400) {
			log (LogWarning, "control '" + c->name () + "' did not take its new value within 2 s");
			break;
		}
		g_usleep (5000);
	}
}

static std::string
m_strip_set (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Route> r = find_route (p["id"]);
	if (!r) {
		return error_reply ("no such strip");
	}
	if (p.count ("gain_db")) {
		double db = atof (p["gain_db"].c_str ());
		/* the fader's range: silence below -60 dB, at most +6 dB */
		double coeff = db <= -60 ? 0 : dB_to_coefficient (std::min (db, 6.0));
		set_control (r->gain_control (), coeff);
	}
	if (p.count ("pan")) {
		if (!r->pan_azimuth_control ()) {
			return error_reply ("this strip has no panner");
		}
		double pan = std::max (-1.0, std::min (1.0, atof (p["pan"].c_str ())));
		set_control (r->pan_azimuth_control (), (pan + 1.0) / 2.0);
	}
	if (p.count ("mute")) {
		set_control (r->mute_control (), truthy (p["mute"]) ? 1 : 0);
	}
	if (p.count ("solo")) {
		if (r->is_master ()) {
			return error_reply ("the master bus cannot be soloed");
		}
		set_control (r->solo_control (), truthy (p["solo"]) ? 1 : 0);
	}
	if (p.count ("rec")) {
		if (!r->rec_enable_control ()) {
			return error_reply ("only tracks can be armed");
		}
		set_control (r->rec_enable_control (), truthy (p["rec"]) ? 1 : 0);
	}
	if (p.count ("name") && !p["name"].empty () && !r->is_master ()) {
		r->set_name (p["name"]);
	}
	return "{\"ok\":true,\"strip\":" + strip_json (r) + "}";
}

static std::string
m_buses_add (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::string name = p["name"].empty () ? std::string ("Bus") : p["name"];
	RouteList added = session->new_audio_route (2, 2, std::shared_ptr<RouteGroup> (), 1, name,
	                                            PresentationInfo::AudioBus, PresentationInfo::max_order);
	if (added.empty ()) {
		return error_reply ("the bus could not be added");
	}
	return "{\"ok\":true,\"strip\":" + strip_json (added.front ()) + "}";
}

static std::string
m_routes_remove (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Route> r = find_route (p["id"]);
	if (!r) {
		return error_reply ("no such track or bus");
	}
	if (r->is_master () || r->is_monitor ()) {
		return error_reply ("the master bus cannot be removed");
	}
	session->remove_route (r);
	return "{\"ok\":true}";
}

static std::string
m_meters ()
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::string j = "{\"ok\":true,\"meters\":[";
	bool first = true;
	for (auto const& r : mixer_routes ()) {
		std::shared_ptr<PeakMeter> meter = r->peak_meter ();
		j += first ? "" : ",";
		j += "{\"id\":" + q (r->id ().to_s ()) + ",\"peak\":[";
		if (meter) {
			ChanCount streams = meter->input_streams ();
			for (uint32_t c = 0; c < streams.n_audio (); ++c) {
				j += (c ? "," : "") + num (meter->meter_level (streams.n_midi () + c, MeterPeak));
			}
		}
		j += "]}";
		first = false;
	}
	j += "]}";
	return j;
}

/* ---------------------------------------------------------------------------
 * timeline
 */

static samplepos_t
param_samples (Params& p, std::string const& key, samplepos_t dflt)
{
	if (!p.count (key) || p[key].empty ()) {
		return dflt;
	}
	return (samplepos_t) std::max (0.0, atof (p[key].c_str ()));
}

static std::string
region_json (std::shared_ptr<Region> const& r)
{
	std::string j = "{\"id\":" + q (r->id ().to_s ());
	j += ",\"name\":" + q (r->name ());
	j += ",\"position\":" + std::to_string ((long long) r->position ().samples ());
	j += ",\"length\":" + std::to_string ((long long) r->length ().samples ());
	j += ",\"start\":" + std::to_string ((long long) r->start ().samples ());
	j += ",\"layer\":" + std::to_string ((long long) r->layer ());
	j += std::string (",\"muted\":") + (r->muted () ? "true" : "false");
	std::shared_ptr<MidiRegion> mr = std::dynamic_pointer_cast<MidiRegion> (r);
	if (mr) {
		j += ",\"kind\":\"midi\"";
		j += ",\"start_beats\":" + num (Temporal::DoubleableBeats (r->start ().beats ()).to_double ());
		j += ",\"length_beats\":" + num (Temporal::DoubleableBeats (r->length ().beats ()).to_double ());
	}
	std::shared_ptr<AudioRegion> ar = std::dynamic_pointer_cast<AudioRegion> (r);
	if (ar) {
		j += ",\"kind\":\"audio\"";
		j += ",\"fade_in\":" + std::to_string ((long long) ar->fade_in_length ().samples ());
		j += ",\"fade_out\":" + std::to_string ((long long) ar->fade_out_length ().samples ());
	}
	j += "}";
	return j;
}

/* a region and the playlist that holds it, looked up by the region's id */
static std::shared_ptr<Region>
find_region (std::string const& id, std::shared_ptr<Playlist>& playlist)
{
	if (id.empty ()) {
		return std::shared_ptr<Region> ();
	}
	PBD::ID rid (id);
	for (auto const& t : tracks_in_order ()) {
		std::shared_ptr<Playlist> pl = t->playlist ();
		if (!pl) {
			continue;
		}
		std::shared_ptr<Region> r = pl->region_by_id (rid);
		if (r) {
			playlist = pl;
			return r;
		}
	}
	return std::shared_ptr<Region> ();
}

/* One undoable edit of one or more playlists, the way the editor records
 * them: the playlists' changes, and their regions', become a single entry in
 * the session's history.
 */
class Edit
{
public:
	Edit (std::string const& name, std::shared_ptr<Playlist> a, std::shared_ptr<Playlist> b = std::shared_ptr<Playlist> ())
	{
		session->begin_reversible_command (name);
		add (a);
		if (b && b != a) {
			add (b);
		}
	}

	~Edit ()
	{
		if (!_done) {
			commit ();
		}
	}

	void commit ()
	{
		_done = true;
		for (auto const& pl : _playlists) {
			pl->rdiff_and_add_command (session);
		}
		session->commit_reversible_command ();
	}

private:
	void add (std::shared_ptr<Playlist> pl)
	{
		pl->clear_changes ();
		pl->clear_owned_changes ();
		_playlists.push_back (pl);
	}

	std::vector<std::shared_ptr<Playlist> > _playlists;
	bool                                    _done = false;
};

static std::string
history_json ()
{
	return ",\"undo\":" + q (session->next_undo ()) + ",\"redo\":" + q (session->next_redo ());
}

static std::string
m_timeline ()
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	samplepos_t end = session->current_end_sample ();
	std::string tracks;
	bool        first = true;
	for (auto const& t : tracks_in_order ()) {
		tracks += first ? "" : ",";
		first = false;
		tracks += "{\"id\":" + q (t->id ().to_s ()) + ",\"name\":" + q (t->name ());
		tracks += ",\"kind\":" + q (strip_kind (t));
		std::shared_ptr<AutomationControl> rec = t->rec_enable_control ();
		tracks += std::string (",\"rec\":") + (rec && rec->get_value () > 0.5 ? "true" : "false");
		tracks += ",\"regions\":[";
		std::shared_ptr<Playlist> pl = t->playlist ();
		if (pl) {
			bool rfirst = true;
			std::shared_ptr<RegionList> regions = pl->region_list ();
			for (auto const& r : *regions) {
				tracks += (rfirst ? "" : ",") + region_json (r);
				rfirst = false;
				end = std::max (end, r->position ().samples () + r->length ().samples ());
			}
		}
		tracks += "]}";
	}
	std::string j = "{\"ok\":true";
	j += ",\"sample_rate\":" + std::to_string ((long long) session->sample_rate ());
	j += ",\"position\":" + std::to_string ((long long) session->transport_sample ());
	j += ",\"end\":" + std::to_string ((long long) end);
	j += history_json ();
	j += ",\"tracks\":[" + tracks + "]}";
	return j;
}

/* Turns finished session sources into a whole-file region and places a copy
 * of it on a track, as one undoable edit.
 */
static std::shared_ptr<Region>
place_sources (SourceList& sources, samplecnt_t length, std::shared_ptr<Track> track, samplepos_t position, std::string const& edit_name)
{
	for (auto const& s : sources) {
		std::shared_ptr<AudioSource> as = std::dynamic_pointer_cast<AudioSource> (s);
		std::shared_ptr<AudioFileSource> afs = std::dynamic_pointer_cast<AudioFileSource> (s);
		afs->flush ();
		{
			Source::WriterLock lock (as->mutex ());
			as->mark_streaming_write_completed (lock, timecnt_t ());
		}
		as->done_with_peakfile_writes ();
		afs->mark_immutable ();
	}

	std::shared_ptr<FileSource> fs = std::dynamic_pointer_cast<FileSource> (sources.front ());
	std::string region_name = region_name_from_path (fs->path (), true, false);

	PBD::PropertyList plist;
	plist.add (ARDOUR::Properties::start, samplepos_t (0));
	plist.add (ARDOUR::Properties::length, length);
	plist.add (ARDOUR::Properties::name, region_name);
	plist.add (ARDOUR::Properties::layer, 0);
	plist.add (ARDOUR::Properties::whole_file, true);
	plist.add (ARDOUR::Properties::external, false);
	plist.add (ARDOUR::Properties::opaque, true);
	std::shared_ptr<Region> whole = RegionFactory::create (sources, plist);

	PBD::PropertyList plist2;
	plist2.add (ARDOUR::Properties::whole_file, false);
	std::shared_ptr<Region> region = RegionFactory::create (whole, plist2);

	std::shared_ptr<Playlist> pl = track->playlist ();
	Edit edit (edit_name, pl);
	pl->add_region (region, timepos_t (position));
	return region;
}

static bool
new_sources (std::shared_ptr<Track> track, uint32_t nchans, std::string const& base, SourceList& sources)
{
	for (uint32_t c = 0; c < nchans; ++c) {
		std::shared_ptr<AudioFileSource> src = session->create_audio_source_for_session (nchans, base, c);
		if (!src) {
			return false;
		}
		sources.push_back (src);
	}
	return true;
}

/* Writes a sine tone into new session sources and places a region of it on
 * an audio track: a way to put material on the timeline without a file
 * or an input to record from.
 */
static std::string
m_regions_tone (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (find_route (p["track"]));
	if (!track || !track->playlist ()) {
		return error_reply ("no such audio track");
	}
	double const sr      = session->sample_rate ();
	double const seconds = p["seconds"].empty () ? 2.0 : atof (p["seconds"].c_str ());
	double const freq    = p["frequency"].empty () ? 440.0 : atof (p["frequency"].c_str ());
	if (seconds <= 0 || seconds > 600) {
		return error_reply ("seconds must be between 0 and 600");
	}
	if (freq <= 0 || freq >= sr / 2) {
		return error_reply ("the frequency is out of range");
	}
	samplepos_t const position = param_samples (p, "position", session->transport_sample ());
	samplecnt_t const length   = (samplecnt_t) (seconds * sr);
	samplecnt_t const fade     = std::min<samplecnt_t> (length / 4, (samplecnt_t) (sr * 0.01));
	uint32_t const    nchans   = std::max<uint32_t> (1, track->n_inputs ().n_audio ());

	SourceList sources;
	if (!new_sources (track, nchans, track->name () + "-tone", sources)) {
		return error_reply ("cannot create an audio file in the session");
	}

	const samplecnt_t  block = 4096;
	std::vector<Sample> buf (block);
	for (auto const& s : sources) {
		std::shared_ptr<AudioSource> as = std::dynamic_pointer_cast<AudioSource> (s);
		for (samplecnt_t done = 0; done < length; done += block) {
			samplecnt_t n = std::min (block, length - done);
			for (samplecnt_t i = 0; i < n; ++i) {
				samplecnt_t k   = done + i;
				double      amp = 0.5;
				if (k < fade) {
					amp *= (double) k / fade;
				} else if (k > length - fade) {
					amp *= (double) (length - k) / fade;
				}
				buf[i] = (Sample) (amp * sin (2.0 * M_PI * freq * k / sr));
			}
			if (as->write (&buf[0], n) != n) {
				return error_reply ("writing the tone failed");
			}
		}
	}

	std::shared_ptr<Region> region = place_sources (sources, length, track, position, "insert tone");
	return "{\"ok\":true,\"region\":" + region_json (region) + "}";
}

/* Copies a sound file (anything libsndfile reads) into the session and puts
 * it on an audio track.  Files at another sample rate are placed as they
 * are; there is no resampling yet.
 */
static std::string
m_regions_import (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<AudioTrack> track = std::dynamic_pointer_cast<AudioTrack> (find_route (p["track"]));
	if (!track || !track->playlist ()) {
		return error_reply ("no such audio track");
	}
	std::string path = p["path"];
	SF_INFO     info;
	memset (&info, 0, sizeof (info));
	SNDFILE* sf = path.empty () ? 0 : sf_open (path.c_str (), SFM_READ, &info);
	if (!sf) {
		return error_reply ("cannot read the sound file: " + std::string (sf_strerror (0)));
	}
	if (info.channels < 1 || info.frames < 1) {
		sf_close (sf);
		return error_reply ("the sound file is empty");
	}

	std::string base = Glib::path_get_basename (path);
	std::string::size_type dot = base.rfind ('.');
	if (dot != std::string::npos && dot > 0) {
		base = base.substr (0, dot);
	}

	/* one session source per file channel */
	uint32_t const nchans = std::min (info.channels, 64);
	SourceList     sources;
	if (!new_sources (track, nchans, base, sources)) {
		sf_close (sf);
		return error_reply ("cannot create an audio file in the session");
	}

	/* a file at another rate is converted to the session's rate on the way
	 * in, with libsamplerate working on the interleaved data */
	bool const   resample = info.samplerate != (int) session->sample_rate ();
	double const ratio    = (double) session->sample_rate () / (double) info.samplerate;
	SRC_STATE*   src      = 0;
	if (resample) {
		int err = 0;
		src = src_new (SRC_SINC_MEDIUM_QUALITY, info.channels, &err);
		if (!src) {
			sf_close (sf);
			return error_reply ("cannot resample: " + std::string (src_strerror (err)));
		}
	}

	const sf_count_t    block = 4096;
	std::vector<float>  interleaved (block * info.channels);
	std::vector<float>  converted (resample ? (size_t) ((block * ratio + 64) * info.channels) : 0);
	std::vector<Sample> chan (std::max<size_t> (block, converted.size () / info.channels + 1));
	sf_count_t          total = 0;

	auto emit = [&] (float const* data, sf_count_t n) {
		for (uint32_t c = 0; c < nchans; ++c) {
			for (sf_count_t i = 0; i < n; ++i) {
				chan[i] = data[i * info.channels + c];
			}
			std::dynamic_pointer_cast<AudioSource> (sources[c])->write (&chan[0], n);
		}
		total += n;
	};

	bool failed = false;
	while (true) {
		sf_count_t n   = sf_readf_float (sf, &interleaved[0], block);
		bool const end = n <= 0;
		if (!resample) {
			if (end) {
				break;
			}
			emit (&interleaved[0], n);
			continue;
		}
		SRC_DATA d;
		memset (&d, 0, sizeof (d));
		d.data_in       = &interleaved[0];
		d.input_frames  = end ? 0 : n;
		d.data_out      = &converted[0];
		d.output_frames = converted.size () / info.channels;
		d.src_ratio     = ratio;
		d.end_of_input  = end ? 1 : 0;
		do {
			if (src_process (src, &d) != 0) {
				failed = true;
				break;
			}
			if (d.output_frames_gen > 0) {
				emit (&converted[0], d.output_frames_gen);
			}
			d.data_in += d.input_frames_used * info.channels;
			d.input_frames -= d.input_frames_used;
		} while (!failed && (d.input_frames > 0 || (end && d.output_frames_gen > 0)));
		if (end || failed) {
			break;
		}
	}
	if (src) {
		src_delete (src);
	}
	sf_close (sf);
	if (failed) {
		return error_reply ("resampling failed");
	}
	if (total == 0) {
		return error_reply ("no audio could be read from the file");
	}

	samplepos_t position = param_samples (p, "position", session->transport_sample ());
	std::shared_ptr<Region> region = place_sources (sources, total, track, position, "import audio");
	std::string j = "{\"ok\":true,\"region\":" + region_json (region);
	if (resample) {
		j += ",\"resampled_from\":" + std::to_string (info.samplerate);
	}
	return j + "}";
}

static std::string
m_region_move (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Playlist> pl;
	std::shared_ptr<Region>   r = find_region (p["id"], pl);
	if (!r) {
		return error_reply ("no such region");
	}
	if (p.count ("track") && !p["track"].empty () && p["track"] != "null") {
		std::shared_ptr<Track> to = std::dynamic_pointer_cast<Track> (find_route (p["track"]));
		if (!to || !to->playlist ()) {
			return error_reply ("no such track");
		}
		if (to->data_type () != r->data_type ()) {
			return error_reply ("the region does not fit that kind of track");
		}
		if (to->playlist () != pl) {
			samplepos_t pos = param_samples (p, "position", r->position ().samples ());
			Edit edit ("move region", pl, to->playlist ());
			pl->remove_region (r);
			to->playlist ()->add_region (r, timepos_t (pos));
			edit.commit ();
			return "{\"ok\":true,\"region\":" + region_json (r) + "}";
		}
	}
	if (p.count ("position")) {
		Edit edit ("move region", pl);
		r->set_position (timepos_t (param_samples (p, "position", 0)));
	}
	return "{\"ok\":true,\"region\":" + region_json (r) + "}";
}

static std::string
m_region_split (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Playlist> pl;
	std::shared_ptr<Region>   r = find_region (p["id"], pl);
	if (!r) {
		return error_reply ("no such region");
	}
	samplepos_t at    = param_samples (p, "position", session->transport_sample ());
	samplepos_t start = r->position ().samples ();
	if (at <= start || at >= start + r->length ().samples ()) {
		return error_reply ("the split point is outside the region");
	}
	Edit edit ("split region", pl);
	pl->split_region (r, timepos_t (at));
	return "{\"ok\":true}";
}

static std::string
m_region_remove (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Playlist> pl;
	std::shared_ptr<Region>   r = find_region (p["id"], pl);
	if (!r) {
		return error_reply ("no such region");
	}
	Edit edit ("remove region", pl);
	pl->remove_region (r);
	return "{\"ok\":true}";
}

static std::string
m_region_mute (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Playlist> pl;
	std::shared_ptr<Region>   r = find_region (p["id"], pl);
	if (!r) {
		return error_reply ("no such region");
	}
	{
		Edit edit (truthy (p["muted"]) ? "mute region" : "unmute region", pl);
		r->set_muted (truthy (p["muted"]));
	}
	return "{\"ok\":true,\"region\":" + region_json (r) + "}";
}

/* moves a region's start and/or end on the timeline, keeping its audio where
 * it is (a trim, not a move)
 */
static std::string
m_region_trim (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Playlist> pl;
	std::shared_ptr<Region>   r = find_region (p["id"], pl);
	if (!r) {
		return error_reply ("no such region");
	}
	samplepos_t const pos = r->position ().samples ();
	samplepos_t const end = pos + r->length ().samples ();
	samplepos_t const new_start = param_samples (p, "start", pos);
	samplepos_t const new_end   = param_samples (p, "end", end);
	if (new_end <= new_start + 64) {
		return error_reply ("the region would be too short");
	}
	{
		Edit edit ("trim region", pl);
		if (new_start != pos) {
			r->trim_front (timepos_t (new_start));
		}
		if (new_end != end) {
			r->trim_end (timepos_t (new_end - 1));
		}
	}
	return "{\"ok\":true,\"region\":" + region_json (r) + "}";
}

static std::string
m_region_fades (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Playlist> pl;
	std::shared_ptr<AudioRegion> r = std::dynamic_pointer_cast<AudioRegion> (find_region (p["id"], pl));
	if (!r) {
		return error_reply ("no such audio region");
	}
	samplecnt_t const len = r->length ().samples ();
	{
		Edit edit ("change fades", pl);
		if (p.count ("fade_in")) {
			r->set_fade_in_active (true);
			r->set_fade_in_length (std::max<samplecnt_t> (64, std::min (len / 2, param_samples (p, "fade_in", 64))));
		}
		if (p.count ("fade_out")) {
			r->set_fade_out_active (true);
			r->set_fade_out_length (std::max<samplecnt_t> (64, std::min (len / 2, param_samples (p, "fade_out", 64))));
		}
	}
	return "{\"ok\":true,\"region\":" + region_json (r) + "}";
}

/* ---------------------------------------------------------------------------
 * MIDI regions and their notes.  Note times are in beats from the start of
 * the region's source; a region's start_beats is where it begins in it.
 */

static std::string
m_regions_midi (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<MidiTrack> track = std::dynamic_pointer_cast<MidiTrack> (find_route (p["track"]));
	if (!track || !track->playlist ()) {
		return error_reply ("no such MIDI track");
	}
	double const beats = p["beats"].empty () ? 4.0 : atof (p["beats"].c_str ());
	if (beats <= 0 || beats > 4096) {
		return error_reply ("beats must be between 0 and 4096");
	}
	timepos_t const pos (param_samples (p, "position", session->transport_sample ()));
	timecnt_t const length (Temporal::Beats::from_double (beats), pos);

	std::shared_ptr<Source> src = session->create_midi_source_by_stealing_name (track);
	if (!src) {
		return error_reply ("cannot create a MIDI file in the session");
	}
	std::shared_ptr<Region> region;
	{
		PBD::PropertyList plist;
		plist.add (ARDOUR::Properties::start, timecnt_t (Temporal::BeatTime));
		plist.add (ARDOUR::Properties::length, length);
		plist.add (ARDOUR::Properties::automatic, true);
		plist.add (ARDOUR::Properties::whole_file, true);
		plist.add (ARDOUR::Properties::name, PBD::basename_nosuffix (src->name ()));
		region = RegionFactory::create (src, plist);
	}
	{
		PBD::PropertyList plist;
		plist.add (ARDOUR::Properties::name, region->name ());
		region = RegionFactory::create (region, plist);
	}
	std::shared_ptr<Playlist> pl = track->playlist ();
	{
		Edit edit ("create MIDI region", pl);
		region->set_position (pos);
		pl->add_region (region, pos, 1.0, false);
	}
	return "{\"ok\":true,\"region\":" + region_json (region) + "}";
}

static std::shared_ptr<MidiRegion>
find_midi_region (Params& p, std::string& err)
{
	err = require_session ();
	if (!err.empty ()) {
		return std::shared_ptr<MidiRegion> ();
	}
	std::shared_ptr<Playlist>   pl;
	std::shared_ptr<MidiRegion> r = std::dynamic_pointer_cast<MidiRegion> (find_region (p["id"], pl));
	if (!r || !r->model ()) {
		err = error_reply ("no such MIDI region");
	}
	return r;
}

static double
beats_d (Temporal::Beats const& b)
{
	return Temporal::DoubleableBeats (b).to_double ();
}

static std::string
m_region_notes (Params& p)
{
	std::string                 e;
	std::shared_ptr<MidiRegion> r = find_midi_region (p, e);
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<MidiModel> model = r->model ();
	std::string j = "{\"ok\":true,\"region\":" + region_json (r) + ",\"notes\":[";
	{
		MidiModel::ReadLock lock (model->read_lock ());
		bool first = true;
		for (auto const& n : model->notes ()) {
			j += first ? "" : ",";
			first = false;
			j += "{\"id\":" + std::to_string ((long long) n->id ());
			j += ",\"note\":" + std::to_string ((int) n->note ());
			j += ",\"velocity\":" + std::to_string ((int) n->velocity ());
			j += ",\"channel\":" + std::to_string ((int) n->channel ());
			j += ",\"start\":" + num (beats_d (n->time ()));
			j += ",\"length\":" + num (beats_d (n->length ())) + "}";
		}
	}
	return j + "]}";
}

static int
param_int (Params& p, std::string const& key, int dflt, int lo, int hi)
{
	if (!p.count (key) || p[key].empty ()) {
		return dflt;
	}
	return std::max (lo, std::min (hi, atoi (p[key].c_str ())));
}

static std::string
m_region_note_add (Params& p)
{
	std::string                 e;
	std::shared_ptr<MidiRegion> r = find_midi_region (p, e);
	if (!e.empty ()) {
		return e;
	}
	double const start  = atof (p["start"].c_str ());
	double const length = p["length"].empty () ? 1.0 : atof (p["length"].c_str ());
	if (start < 0 || length <= 0) {
		return error_reply ("start and length must be positive beats");
	}
	typedef Evoral::Note<Temporal::Beats> Note;
	std::shared_ptr<Note> n (new Note ((uint8_t) param_int (p, "channel", 0, 0, 15),
	                                   Temporal::Beats::from_double (start),
	                                   Temporal::Beats::from_double (length),
	                                   (uint8_t) param_int (p, "note", 60, 0, 127),
	                                   (uint8_t) param_int (p, "velocity", 100, 1, 127)));
	std::shared_ptr<MidiModel> model = r->model ();
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command ("add note");
	cmd->add (n);
	model->apply_diff_command_as_commit (*session, cmd);
	return "{\"ok\":true,\"id\":" + std::to_string ((long long) n->id ()) + "}";
}

static std::string
m_region_note_edit (Params& p, bool remove)
{
	std::string                 e;
	std::shared_ptr<MidiRegion> r = find_midi_region (p, e);
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<MidiModel> model = r->model ();
	MidiModel::NotePtr         n = model->find_note ((Evoral::event_id_t) atoll (p["note_id"].c_str ()));
	if (!n) {
		return error_reply ("no such note");
	}
	MidiModel::NoteDiffCommand* cmd = model->new_note_diff_command (remove ? "remove note" : "edit note");
	if (remove) {
		cmd->remove (n);
	} else {
		typedef MidiModel::NoteDiffCommand C;
		if (!p["note"].empty ()) {
			cmd->change (n, C::NoteNumber, (uint8_t) param_int (p, "note", 60, 0, 127));
		}
		if (!p["velocity"].empty ()) {
			cmd->change (n, C::Velocity, (uint8_t) param_int (p, "velocity", 100, 1, 127));
		}
		if (!p["start"].empty ()) {
			cmd->change (n, C::StartTime, Temporal::Beats::from_double (std::max (0.0, atof (p["start"].c_str ()))));
		}
		if (!p["length"].empty ()) {
			cmd->change (n, C::Length, Temporal::Beats::from_double (std::max (1.0 / 64, atof (p["length"].c_str ()))));
		}
	}
	model->apply_diff_command_as_commit (*session, cmd);
	return "{\"ok\":true}";
}

/* ---------------------------------------------------------------------------
 * plugins on routes
 */

static std::string
m_plugins_list (Params& p)
{
	bool const instruments = p["instruments"] == "true";
	std::string j = "{\"ok\":true,\"plugins\":[";
	bool first = true;
	for (auto const& i : all_plugins ()) {
		if (instruments && !i->is_instrument ()) {
			continue;
		}
		j += first ? "" : ",";
		first = false;
		j += "{\"id\":" + q (i->unique_id) + ",\"name\":" + q (i->name);
		j += ",\"type\":" + q (plugin_type_name (i->type)) + ",\"category\":" + q (i->category);
		j += ",\"creator\":" + q (i->creator);
		j += std::string (",\"instrument\":") + (i->is_instrument () ? "true" : "false") + "}";
	}
	return j + "]}";
}

static std::shared_ptr<PluginInsert>
find_insert (Params& p, std::shared_ptr<Route>& route, std::string& err)
{
	err = require_session ();
	if (!err.empty ()) {
		return std::shared_ptr<PluginInsert> ();
	}
	route = find_route (p["route"]);
	if (!route) {
		err = error_reply ("no such track or bus");
		return std::shared_ptr<PluginInsert> ();
	}
	std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (route->processor_by_id (PBD::ID (p["plugin"])));
	if (!pi) {
		err = error_reply ("no such plugin on that route");
	}
	return pi;
}

static std::string
plugin_detail_json (std::shared_ptr<PluginInsert> const& pi)
{
	std::string j = plugin_brief_json (pi);
	j.pop_back ();
	j += ",\"params\":[";
	std::shared_ptr<Plugin> plugin = pi->plugin ();
	bool first = true;
	for (uint32_t i = 0; i < plugin->parameter_count (); ++i) {
		if (!plugin->parameter_is_control (i) || !plugin->parameter_is_input (i)) {
			continue;
		}
		std::shared_ptr<AutomationControl> ac = pi->automation_control (Evoral::Parameter (PluginAutomation, 0, i));
		if (!ac) {
			continue;
		}
		ParameterDescriptor const& d = ac->desc ();
		j += first ? "" : ",";
		first = false;
		j += "{\"index\":" + std::to_string (i);
		j += ",\"name\":" + q (d.label.empty () ? plugin->parameter_label (i) : d.label);
		j += ",\"value\":" + num (ac->get_value ());
		j += ",\"min\":" + num (d.lower) + ",\"max\":" + num (d.upper);
		j += std::string (",\"toggled\":") + (d.toggled ? "true" : "false");
		j += std::string (",\"integer\":") + (d.integer_step || d.enumeration ? "true" : "false");
		j += std::string (",\"log\":") + (d.logarithmic ? "true" : "false");
		j += ",\"text\":" + q (ac->get_user_string ()) + "}";
	}
	return j + "]}";
}

static std::string
m_plugin_add (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Route> route = find_route (p["route"]);
	if (!route) {
		return error_reply ("no such track or bus");
	}
	PluginInfoPtr info = find_plugin_info (p["id"]);
	if (!info) {
		return error_reply ("no such plugin: " + p["id"]);
	}
	PluginPtr plugin = info->load (*session);
	if (!plugin) {
		return error_reply ("the plugin did not load");
	}
	std::shared_ptr<Processor> pi (new PluginInsert (*session, *route, plugin));
	Route::ProcessorStreams err;
	if (route->add_processor (pi, p["post"] == "true" ? PostFader : PreFader, &err) != 0) {
		return error_reply ("the plugin does not fit this route's channels");
	}
	return "{\"ok\":true,\"plugin\":" + plugin_detail_json (std::dynamic_pointer_cast<PluginInsert> (pi)) + "}";
}

static std::string
m_plugin_get (Params& p)
{
	std::shared_ptr<Route>        route;
	std::string                   e;
	std::shared_ptr<PluginInsert> pi = find_insert (p, route, e);
	if (!e.empty ()) {
		return e;
	}
	return "{\"ok\":true,\"plugin\":" + plugin_detail_json (pi) + "}";
}

static std::string
m_plugin_set (Params& p)
{
	std::shared_ptr<Route>        route;
	std::string                   e;
	std::shared_ptr<PluginInsert> pi = find_insert (p, route, e);
	if (!e.empty ()) {
		return e;
	}
	if (p.count ("active") && !p["active"].empty ()) {
		pi->enable (truthy (p["active"]));
	}
	if (p.count ("param") && !p["param"].empty ()) {
		std::shared_ptr<AutomationControl> ac =
			pi->automation_control (Evoral::Parameter (PluginAutomation, 0, (uint32_t) atoi (p["param"].c_str ())));
		if (!ac) {
			return error_reply ("no such parameter");
		}
		ParameterDescriptor const& d = ac->desc ();
		double v = std::max<double> (d.lower, std::min<double> (d.upper, atof (p["value"].c_str ())));
		ac->set_value (v, PBD::Controllable::NoGroup);
	}
	return "{\"ok\":true,\"plugin\":" + plugin_detail_json (pi) + "}";
}

static std::string
m_plugin_remove (Params& p)
{
	std::shared_ptr<Route>        route;
	std::string                   e;
	std::shared_ptr<PluginInsert> pi = find_insert (p, route, e);
	if (!e.empty ()) {
		return e;
	}
	if (route->remove_processor (pi) != 0) {
		return error_reply ("the plugin could not be removed");
	}
	return "{\"ok\":true}";
}

/* ---------------------------------------------------------------------------
 * MIDI input
 */

static int
hexval (char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

/* the length of the message a status byte starts, 0 for sysex */
static size_t
midi_message_length (uint8_t status)
{
	switch (status & 0xf0) {
		case 0xc0:
		case 0xd0:
			return 2;
		case 0xf0:
			switch (status) {
				case 0xf0: return 0;
				case 0xf1: case 0xf3: return 2;
				case 0xf2: return 3;
				default: return 1;
			}
		default:
			return 3;
	}
}

static std::string
m_midi_send (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::vector<uint8_t> bytes;
	if (!p["data"].empty ()) {
		std::string const& d = p["data"];
		int hi = -1;
		for (char c : d) {
			int v = hexval (c);
			if (v < 0) {
				continue;
			}
			if (hi < 0) {
				hi = v;
			} else {
				bytes.push_back ((uint8_t) (hi * 16 + v));
				hi = -1;
			}
		}
	} else {
		int const note = atoi (p["note"].c_str ()) & 0x7f;
		int const vel  = p["velocity"].empty () ? 100 : (atoi (p["velocity"].c_str ()) & 0x7f);
		int const chan = atoi (p["channel"].c_str ()) & 0x0f;
		bool const on  = p["on"] != "false";
		bytes.push_back ((uint8_t) ((on ? 0x90 : 0x80) | chan));
		bytes.push_back ((uint8_t) note);
		bytes.push_back ((uint8_t) (on ? vel : 0));
	}

	/* make sure the tracks hear it */
	if (!p["track"].empty ()) {
		connect_midi_input (std::dynamic_pointer_cast<Track> (find_route (p["track"])));
	} else {
		for (auto const& t : tracks_in_order ()) {
			if (std::dynamic_pointer_cast<MidiTrack> (t)) {
				connect_midi_input (t);
			}
		}
	}

	std::shared_ptr<AsyncMIDIPort> vk = session->vkbd_output_port ();
	if (!vk) {
		return error_reply ("there is no MIDI input port");
	}
	/* one message per write; running status is expanded */
	size_t  i = 0, sent = 0;
	uint8_t running = 0;
	while (i < bytes.size ()) {
		uint8_t b = bytes[i];
		if (b == 0xf0) {
			size_t j = i;
			while (j < bytes.size () && bytes[j] != 0xf7) ++j;
			if (j < bytes.size ()) {
				vk->write (&bytes[i], j - i + 1, 0);
				++sent;
			}
			i = j + 1;
			continue;
		}
		uint8_t msg[3];
		size_t  len;
		if (b & 0x80) {
			if (b < 0xf0) {
				running = b;
			}
			len = midi_message_length (b);
			if (i + len > bytes.size ()) break;
			memcpy (msg, &bytes[i], len);
			i += len;
		} else if (running) {
			len = midi_message_length (running);
			if (i + len - 1 > bytes.size ()) break;
			msg[0] = running;
			memcpy (msg + 1, &bytes[i], len - 1);
			i += len - 1;
		} else {
			++i;
			continue;
		}
		vk->write (msg, len, 0);
		++sent;
	}
	return "{\"ok\":true,\"messages\":" + std::to_string (sent) + "}";
}

/* ---------------------------------------------------------------------------
 * plugin presets
 */

static std::string
presets_json (std::shared_ptr<PluginInsert> const& pi)
{
	std::shared_ptr<Plugin> plugin = pi->plugin ();
	std::string j = "{\"ok\":true,\"current\":" + q (plugin->last_preset ().uri) + ",\"presets\":[";
	bool first = true;
	for (auto const& r : plugin->get_presets ()) {
		j += first ? "" : ",";
		first = false;
		j += "{\"uri\":" + q (r.uri) + ",\"label\":" + q (r.label);
		j += std::string (",\"user\":") + (r.user ? "true" : "false") + "}";
	}
	return j + "]}";
}

static std::string
m_plugin_presets (Params& p)
{
	std::shared_ptr<Route>        route;
	std::string                   e;
	std::shared_ptr<PluginInsert> pi = find_insert (p, route, e);
	if (!e.empty ()) {
		return e;
	}
	return presets_json (pi);
}

static std::string
m_plugin_preset_load (Params& p)
{
	std::shared_ptr<Route>        route;
	std::string                   e;
	std::shared_ptr<PluginInsert> pi = find_insert (p, route, e);
	if (!e.empty ()) {
		return e;
	}
	Plugin::PresetRecord const* r = pi->plugin ()->preset_by_uri (p["uri"]);
	if (!r) {
		return error_reply ("no such preset");
	}
	if (!pi->load_preset (*r)) {
		return error_reply ("the preset did not load");
	}
	return "{\"ok\":true,\"plugin\":" + plugin_detail_json (pi) + "}";
}

static std::string
m_plugin_preset_save (Params& p)
{
	std::shared_ptr<Route>        route;
	std::string                   e;
	std::shared_ptr<PluginInsert> pi = find_insert (p, route, e);
	if (!e.empty ()) {
		return e;
	}
	if (p["name"].empty ()) {
		return error_reply ("a preset needs a name");
	}
	Plugin::PresetRecord r = pi->plugin ()->save_preset (p["name"]);
	if (!r.valid) {
		return error_reply ("the preset could not be saved");
	}
	return presets_json (pi);
}

/* ---------------------------------------------------------------------------
 * automation: control is gain, trim, pan, mute, or plugin (with plugin and
 * param).  Points are {t: samples, value, y: 0..1 as a fader shows it}.
 */

static std::shared_ptr<AutomationControl>
find_automation (Params& p, std::string& err)
{
	err = require_session ();
	if (!err.empty ()) {
		return std::shared_ptr<AutomationControl> ();
	}
	std::shared_ptr<Route> route = find_route (p["route"]);
	if (!route) {
		err = error_reply ("no such track or bus");
		return std::shared_ptr<AutomationControl> ();
	}
	std::string const&                 c = p["control"];
	std::shared_ptr<AutomationControl> ac;
	if (c.empty () || c == "gain") {
		ac = route->gain_control ();
	} else if (c == "trim") {
		ac = route->trim_control ();
	} else if (c == "pan") {
		ac = route->pan_azimuth_control ();
	} else if (c == "mute") {
		ac = route->mute_control ();
	} else if (c == "plugin") {
		std::shared_ptr<PluginInsert> pi = std::dynamic_pointer_cast<PluginInsert> (route->processor_by_id (PBD::ID (p["plugin"])));
		if (pi) {
			ac = pi->automation_control (Evoral::Parameter (PluginAutomation, 0, (uint32_t) atoi (p["param"].c_str ())));
		}
	}
	if (!ac || !ac->alist ()) {
		err = error_reply ("that control cannot be automated");
		return std::shared_ptr<AutomationControl> ();
	}
	return ac;
}

static const char*
auto_state_name (AutoState s)
{
	switch (s) {
		case Write: return "write";
		case Touch: return "touch";
		case Play:  return "play";
		case Latch: return "latch";
		default:    return "manual";
	}
}

static std::string
automation_json (std::shared_ptr<AutomationControl> const& ac)
{
	std::shared_ptr<AutomationList> al = ac->alist ();
	ParameterDescriptor const&      d  = ac->desc ();
	std::string j = "{\"ok\":true,\"state\":" + q (auto_state_name (al->automation_state ()));
	j += ",\"name\":" + q (ac->name ());
	j += ",\"min\":" + num (d.lower) + ",\"max\":" + num (d.upper);
	j += ",\"value\":" + num (ac->get_value ()) + ",\"y\":" + num (ac->internal_to_interface (ac->get_value ()));
	j += ",\"points\":[";
	bool first = true;
	for (auto const& ev : al->events ()) {
		j += first ? "" : ",";
		first = false;
		j += "{\"t\":" + std::to_string ((long long) ev->when.samples ());
		j += ",\"value\":" + num (ev->value) + ",\"y\":" + num (ac->internal_to_interface (ev->value)) + "}";
	}
	return j + "]}";
}

static std::string
m_automation_get (Params& p)
{
	std::string                        e;
	std::shared_ptr<AutomationControl> ac = find_automation (p, e);
	if (!e.empty ()) {
		return e;
	}
	return automation_json (ac);
}

/* an undoable change of an automation list */
template <typename F>
static void
automation_edit (std::shared_ptr<AutomationList> const& al, std::string const& name, F change)
{
	XMLNode& before = al->get_state ();
	change ();
	XMLNode& after = al->get_state ();
	session->begin_reversible_command (name);
	session->add_command (new MementoCommand<AutomationList> (*al, &before, &after));
	session->commit_reversible_command ();
}

static std::string
m_automation_edit (Params& p, std::string const& what)
{
	std::string                        e;
	std::shared_ptr<AutomationControl> ac = find_automation (p, e);
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<AutomationList> al = ac->alist ();
	if (what == "state") {
		std::string const& s = p["state"];
		AutoState as = s == "play" ? Play : s == "write" ? Write : s == "touch" ? Touch : s == "latch" ? Latch : Off;
		ac->set_automation_state (as);
	} else if (what == "add") {
		double v;
		if (!p["y"].empty ()) {
			v = ac->interface_to_internal (std::max (0.0, std::min (1.0, atof (p["y"].c_str ()))));
		} else {
			v = atof (p["value"].c_str ());
		}
		ParameterDescriptor const& d = ac->desc ();
		v = std::max<double> (d.lower, std::min<double> (d.upper, v));
		timepos_t const when (std::max<samplepos_t> (0, atoll (p["t"].c_str ())));
		automation_edit (al, "add automation point", [&] () { al->editor_add (when, v, false); });
	} else if (what == "remove") {
		int const idx = atoi (p["index"].c_str ());
		if (idx < 0 || idx >= (int) al->events ().size ()) {
			return error_reply ("no such point");
		}
		automation_edit (al, "remove automation point", [&] () {
			AutomationList::iterator i = al->begin ();
			std::advance (i, idx);
			al->erase (i);
		});
	} else if (what == "clear") {
		automation_edit (al, "clear automation", [&] () { al->clear (); });
	}
	return automation_json (ac);
}

static std::string
m_history (bool undo)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	if (undo) {
		if (session->undo_depth () == 0) {
			return error_reply ("nothing to undo");
		}
		session->undo (1);
	} else {
		if (session->redo_depth () == 0) {
			return error_reply ("nothing to redo");
		}
		session->redo (1);
	}
	return "{\"ok\":true" + history_json () + "}";
}

/* arms the session for recording (armed tracks record once it rolls), or
 * disarms it
 */
static std::string
m_transport_record (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	if (truthy (p["enabled"])) {
		session->maybe_enable_record ();
	} else {
		session->disable_record (false);
	}
	for (int i = 0; i < 200 && (session->record_status () >= ARDOUR::Enabled) != truthy (p["enabled"]); ++i) {
		g_usleep (5000);
	}
	return transport_json ();
}

/* the waveform of a region's first channel as count min/max pairs */
static std::string
m_region_peaks (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::shared_ptr<Playlist> pl;
	std::shared_ptr<AudioRegion> r = std::dynamic_pointer_cast<AudioRegion> (find_region (p["id"], pl));
	if (!r) {
		return error_reply ("no such audio region");
	}
	int count = p["count"].empty () ? 200 : atoi (p["count"].c_str ());
	count     = std::max (1, std::min (count, 4000));
	/* read the audio itself rather than the peak files, which are built in
	 * the background and may not exist yet; a long region is sampled, at most
	 * 4096 samples per bucket
	 */
	samplecnt_t const len    = r->length ().samples ();
	samplecnt_t const bucket = std::max<samplecnt_t> (1, len / count);
	samplecnt_t const span   = std::min<samplecnt_t> (bucket, 4096);
	std::vector<Sample> buf (span);
	std::string j = "{\"ok\":true,\"peaks\":[";
	for (int i = 0; i < count && i * bucket < len; ++i) {
		samplecnt_t n   = std::min (span, len - i * bucket);
		samplecnt_t got = r->read (&buf[0], i * bucket, n, 0);
		float lo = 0, hi = 0;
		for (samplecnt_t k = 0; k < got; ++k) {
			lo = std::min (lo, buf[k]);
			hi = std::max (hi, buf[k]);
		}
		j += (i ? "," : "") + num (lo) + "," + num (hi);
	}
	j += "]}";
	return j;
}

static std::string
m_transport_locate (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	session->request_locate (param_samples (p, "position", 0), false, RollIfAppropriate, TRS_UI);
	return transport_json ();
}

static std::string
dispatch (std::string const& method, Params& p)
{
	if (method == "engine.status") {
		return status_json ();
	}
	if (!initialized) {
		return error_reply ("the engine is not initialized");
	}
	if (method == "session.list")    return m_session_list ();
	if (method == "session.create")  return m_session_create (p);
	if (method == "session.open")    return m_session_open (p);
	if (method == "session.save")    return m_session_save ();
	if (method == "session.close")   { close_session (); return status_json (); }
	if (method == "transport.play")  return m_transport (true);
	if (method == "transport.stop")  return m_transport (false);
	if (method == "transport.state") return transport_json ();
	if (method == "tracks.list")     return m_tracks_list ();
	if (method == "tracks.add")      return m_tracks_add (p);
	if (method == "meters")          return m_meters ();
	if (method == "tracks.remove")   return m_routes_remove (p);
	if (method == "buses.add")       return m_buses_add (p);
	if (method == "mixer.strips")    return m_mixer_strips ();
	if (method == "strip.set")       return m_strip_set (p);
	if (method == "timeline")        return m_timeline ();
	if (method == "regions.tone")    return m_regions_tone (p);
	if (method == "region.move")     return m_region_move (p);
	if (method == "region.split")    return m_region_split (p);
	if (method == "region.remove")   return m_region_remove (p);
	if (method == "region.mute")     return m_region_mute (p);
	if (method == "region.trim")     return m_region_trim (p);
	if (method == "region.fades")    return m_region_fades (p);
	if (method == "regions.import")  return m_regions_import (p);
	if (method == "regions.midi")    return m_regions_midi (p);
	if (method == "region.notes")    return m_region_notes (p);
	if (method == "region.note.add") return m_region_note_add (p);
	if (method == "region.note.edit") return m_region_note_edit (p, false);
	if (method == "region.note.remove") return m_region_note_edit (p, true);
	if (method == "plugins.list")    return m_plugins_list (p);
	if (method == "plugin.add")      return m_plugin_add (p);
	if (method == "plugin.get")      return m_plugin_get (p);
	if (method == "plugin.set")      return m_plugin_set (p);
	if (method == "plugin.remove")   return m_plugin_remove (p);
	if (method == "midi.send")       return m_midi_send (p);
	if (method == "plugin.presets")  return m_plugin_presets (p);
	if (method == "plugin.preset.load") return m_plugin_preset_load (p);
	if (method == "plugin.preset.save") return m_plugin_preset_save (p);
	if (method == "automation.get")  return m_automation_get (p);
	if (method == "automation.state") return m_automation_edit (p, "state");
	if (method == "automation.add")  return m_automation_edit (p, "add");
	if (method == "automation.remove") return m_automation_edit (p, "remove");
	if (method == "automation.clear") return m_automation_edit (p, "clear");
	if (method == "edit.undo")       return m_history (true);
	if (method == "edit.redo")       return m_history (false);
	if (method == "transport.record") return m_transport_record (p);
	if (method == "region.peaks")    return m_region_peaks (p);
	if (method == "transport.locate") return m_transport_locate (p);
	return error_reply ("unknown method '" + method + "'");
}

/* ---------------------------------------------------------------------------
 * entry points
 */

std::string
init (Paths const& paths, std::string const& backend, std::string const& fallback_backend)
{
	return run_on_engine_thread ([=] {
		backend_names[0] = backend;
		backend_names[1] = fallback_backend;
		return do_init (paths);
	});
}

std::string
call (std::string const& method, std::string const& params)
{
	Params p;
	if (!parse_params (params, p)) {
		return error_reply ("the parameters are not a flat JSON object");
	}
	return run_on_engine_thread ([=] () mutable { return dispatch (method, p); });
}

} /* namespace ArdourBridge */
