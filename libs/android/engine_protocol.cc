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
#include <map>
#include <memory>
#include <mutex>
#include <vector>

#include <glib.h>
#include <glib/gstdio.h>
#include <glibmm/fileutils.h>
#include <glibmm/miscutils.h>

#include "pbd/error.h"
#include "pbd/event_loop.h"
#include "pbd/failed_constructor.h"
#include "pbd/receiver.h"
#include "pbd/rwlock.h"
#include "pbd/transmitter.h"

#include "ardour/ardour.h"
#include "ardour/audio_track.h"
#include "ardour/audioengine.h"
#include "ardour/filename_extensions.h"
#include "ardour/meter.h"
#include "ardour/presentation_info.h"
#include "ardour/route.h"
#include "ardour/session.h"
#include "ardour/session_event.h"
#include "ardour/types.h"

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

class LogReceiver : public PBD::Receiver
{
protected:
	void receive (PBD::Transmitter::Channel chn, const char* str)
	{
		switch (chn) {
			case PBD::Transmitter::Debug:
				return;
			case PBD::Transmitter::Info:
				log (LogInfo, str);
				break;
			case PBD::Transmitter::Warning:
				log (LogWarning, str);
				break;
			case PBD::Transmitter::Error:
			case PBD::Transmitter::Fatal:
			case PBD::Transmitter::Throw:
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

	bool call_slot (PBD::EventLoop::InvalidationRecord*, const std::function<void()>& f)
	{
		if (pthread_equal (pthread_self (), _thread)) {
			f ();
		}
		return true;
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
		} catch (PBD::failed_constructor const&) {
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

static std::string
m_tracks_add (Params& p)
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
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

static std::string
m_meters ()
{
	std::string e = require_session ();
	if (!e.empty ()) {
		return e;
	}
	std::string j = "{\"ok\":true,\"meters\":[";
	bool first = true;
	std::shared_ptr<RouteList> tracks = session->get_tracks ();
	for (auto const& r : *tracks) {
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
