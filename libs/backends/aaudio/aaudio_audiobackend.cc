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

#include <cmath>
#include <cstdlib>
#include <cstring>
#include <algorithm>

#include <glibmm.h>
#include <pthread.h>

#include "aaudio_audiobackend.h"

#include "pbd/error.h"
#include "pbd/compose.h"
#include "pbd/pthread_utils.h"
#include "pbd/i18n.h"

#include "ardour/debug.h"
#include "ardour/port_manager.h"

using namespace ARDOUR;

static std::string s_instance_name;

/* the DSP load is measured with the monotonic clock of glib */
static int64_t
_x_get_monotonic_usec ()
{
	return g_get_monotonic_time ();
}

/* ---------------------------------------------------------------------- */
/* ports                                                                   */
/* ---------------------------------------------------------------------- */

AAudioAudioPort::AAudioAudioPort (AAudioBackend& b, const std::string& name, PortFlags flags)
	: BackendPort (b, name, flags)
{
	memset (_buffer, 0, sizeof (_buffer));
}

AAudioAudioPort::~AAudioAudioPort ()
{
}

void*
AAudioAudioPort::get_buffer (pframes_t nframes)
{
	if (nframes > max_buffer_size) {
		nframes = max_buffer_size;
	}

	if (is_input ()) {
		const std::set<BackendPortPtr>& connections = get_connections ();
		std::set<BackendPortPtr>::const_iterator it = connections.begin ();
		if (it == connections.end ()) {
			memset (_buffer, 0, nframes * sizeof (Sample));
		} else {
			std::shared_ptr<const AAudioAudioPort> source = std::dynamic_pointer_cast<const AAudioAudioPort> (*it);
			assert (source && source->is_output ());
			memcpy (_buffer, source->const_buffer (), nframes * sizeof (Sample));
			while (++it != connections.end ()) {
				source = std::dynamic_pointer_cast<const AAudioAudioPort> (*it);
				assert (source && source->is_output ());
				const Sample* src = source->const_buffer ();
				for (pframes_t s = 0; s < nframes; ++s) {
					_buffer[s] += src[s];
				}
			}
		}
	}
	return _buffer;
}

/* ---------------------------------------------------------------------- */
/* backend                                                                 */
/* ---------------------------------------------------------------------- */

size_t AAudioBackend::_max_buffer_size = 8192;
std::vector<AudioBackend::DeviceStatus> AAudioBackend::_device_status;

AAudioBackend::AAudioBackend (AudioEngine& e, AudioBackendInfo& info)
	: AudioBackend (e, info)
	, PortEngineSharedImpl (e, s_instance_name)
	, _running (false)
	, _freewheel (false)
	, _freewheeling (false)
	, _realtime (true)
	, _samplerate (48000.0f)
	, _samples_per_period (1024)
	, _dsp_load (0)
	, _n_inputs (0)
	, _n_outputs (2)
	, _systemic_input_latency (0)
	, _systemic_output_latency (0)
	, _interleaved (true)
	, _processed_samples (0)
	, _output_device (0)
	, _input_device (0)
{
	_instance_name = s_instance_name;
}

AAudioBackend::~AAudioBackend ()
{
	close_devices ();
}

std::string
AAudioBackend::name () const
{
	return X_("AAudio");
}

bool
AAudioBackend::is_realtime () const
{
	return true;
}

std::vector<AudioBackend::DeviceStatus>
AAudioBackend::enumerate_devices () const
{
	if (_device_status.empty ()) {

		std::vector<AudioBackend::DeviceStatus> devices;
		char buffer[4096];

		if (aa_device_get_device_list (buffer, sizeof (buffer)) > 0) {

			/* the list is "id: name", one device per line */
			std::string list (buffer);
			size_t pos = 0;

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
				std::string device_name = (colon == std::string::npos) ? line : line.substr (colon + 1);

				while (!device_name.empty () && device_name[0] == ' ') {
					device_name.erase (0, 1);
				}

				devices.push_back (DeviceStatus (device_name, true));
			}
		}

		if (devices.empty ()) {
			/* AAudio only exposes the device list from API 30 on; before
			 * that there is only the default device of each direction */
			devices.push_back (DeviceStatus (_("Default"), true));
		}

		_device_status = devices;
	}

	return _device_status;
}

std::vector<float>
AAudioBackend::available_sample_rates (const std::string& device) const
{
	std::vector<float> rates;
	rates.push_back (44100.0f);
	rates.push_back (48000.0f);
	rates.push_back (96000.0f);
	return rates;
}

std::vector<uint32_t>
AAudioBackend::available_buffer_sizes (const std::string& device) const
{
	std::vector<uint32_t> sizes;
	sizes.push_back (128);
	sizes.push_back (256);
	sizes.push_back (512);
	sizes.push_back (1024);
	sizes.push_back (2048);
	sizes.push_back (4096);
	return sizes;
}

int
AAudioBackend::set_device_name (const std::string& name)
{
	_device = name;
	return 0;
}

int
AAudioBackend::set_sample_rate (float sr)
{
	if (sr <= 0) {
		return -1;
	}
	_samplerate = sr;
	engine.sample_rate_change (sr);
	return 0;
}

int
AAudioBackend::set_buffer_size (uint32_t bs)
{
	if (bs <= 0 || bs > _max_buffer_size) {
		return -1;
	}
	_samples_per_period = bs;

	LatencyRange lr;
	lr.min = lr.max = _systemic_input_latency;
	for (auto it = _system_inputs.begin (); it != _system_inputs.end (); ++it) {
		set_latency_range (*it, false, lr);
	}
	lr.min = lr.max = _systemic_output_latency;
	for (auto it = _system_outputs.begin (); it != _system_outputs.end (); ++it) {
		set_latency_range (*it, true, lr);
	}

	engine.buffer_size_change (bs);
	return 0;
}

int
AAudioBackend::set_interleaved (bool yn)
{
	/* the AAudio PCM_FLOAT format is interleaved, and there is no planar
	 * variant of it */
	if (!yn) {
		return -1;
	}
	_interleaved = true;
	return 0;
}

int
AAudioBackend::set_systemic_input_latency (uint32_t lat)
{
	_systemic_input_latency = lat;
	return 0;
}

int
AAudioBackend::set_systemic_output_latency (uint32_t lat)
{
	_systemic_output_latency = lat;
	return 0;
}

std::string
AAudioBackend::device_name () const
{
	return _device;
}

float
AAudioBackend::sample_rate () const
{
	return _samplerate;
}

uint32_t
AAudioBackend::buffer_size () const
{
	return _samples_per_period;
}

bool
AAudioBackend::interleaved () const
{
	return _interleaved;
}

uint32_t
AAudioBackend::systemic_input_latency () const
{
	return _systemic_input_latency;
}

uint32_t
AAudioBackend::systemic_output_latency () const
{
	return _systemic_output_latency;
}

int
AAudioBackend::freewheel (bool start_stop)
{
	_freewheeling = start_stop;
	return 0;
}

float
AAudioBackend::dsp_load () const
{
	return 100.f * _dsp_load;
}

size_t
AAudioBackend::raw_buffer_size (DataType t)
{
	switch (t) {
	case DataType::AUDIO:
		return _samples_per_period * sizeof (Sample);
	case DataType::MIDI:
		return _max_buffer_size;
	}
	return 0;
}

samplepos_t
AAudioBackend::sample_time ()
{
	return _processed_samples;
}

samplepos_t
AAudioBackend::sample_time_at_cycle_start ()
{
	return _processed_samples;
}

pframes_t
AAudioBackend::samples_since_cycle_start ()
{
	return 0;
}

int
AAudioBackend::create_process_thread (std::function<void()> func)
{
	pthread_t thread_id;
	ThreadData* td = new ThreadData (this, func, PBD_RT_STACKSIZE_PROC);

	bool ok = _realtime && 0 == pbd_realtime_pthread_create ("AAudio Proc", PBD_SCHED_FIFO, PBD_RT_PRI_PROC,
			PBD_RT_STACKSIZE_PROC, &thread_id, aaudio_process_thread, td);

	if (!ok && pbd_pthread_create (PBD_RT_STACKSIZE_PROC, &thread_id, aaudio_process_thread, td)) {
		PBD::error << _("AAudioBackend: cannot create process thread.") << endmsg;
		delete td;
		return -1;
	}

	_threads.push_back (thread_id);
	return 0;
}

int
AAudioBackend::join_process_threads ()
{
	for (auto const& t : _threads) {
		void* status;
		pthread_join (t, &status);
	}
	_threads.clear ();
	return 0;
}

bool
AAudioBackend::in_process_thread ()
{
	return pthread_equal (_main_thread, pthread_self ());
}

uint32_t
AAudioBackend::process_thread_count ()
{
	return _threads.size ();
}

void
AAudioBackend::update_latencies ()
{
	port_connect_add_remove_callback ();
}

void*
AAudioBackend::private_handle () const
{
	return 0;
}

const std::string&
AAudioBackend::my_name () const
{
	return _instance_name;
}

int
AAudioBackend::midi_event_get (pframes_t& timestamp, size_t& size, uint8_t const** buf, void* port_buffer, uint32_t event_index)
{
	return 0;
}

int
AAudioBackend::midi_event_put (void* port_buffer, pframes_t timestamp, const uint8_t* buffer, size_t size)
{
	return 0;
}

uint32_t
AAudioBackend::get_midi_event_count (void* port_buffer)
{
	return 0;
}

void
AAudioBackend::midi_clear (void* port_buffer)
{
}

bool
AAudioBackend::can_monitor_input () const
{
	return false;
}

int
AAudioBackend::request_input_monitoring (PortHandle, bool)
{
	return -1;
}

int
AAudioBackend::ensure_input_monitoring (PortHandle, bool)
{
	return -1;
}

bool
AAudioBackend::monitoring_input (PortHandle)
{
	return false;
}

void
AAudioBackend::set_latency_range (PortEngine::PortHandle port_handle, bool for_playback, LatencyRange latency_range)
{
	BackendPortPtr port = std::dynamic_pointer_cast<BackendPort> (port_handle);

	if (!valid_port (port)) {
		return;
	}

	port->set_latency_range (latency_range, for_playback);
}

LatencyRange
AAudioBackend::get_latency_range (PortEngine::PortHandle port_handle, bool for_playback)
{
	LatencyRange r;
	BackendPortPtr port = std::dynamic_pointer_cast<BackendPort> (port_handle);

	if (!valid_port (port)) {
		r.min = 0;
		r.max = 0;
		return r;
	}

	return port->latency_range (for_playback);
}

void*
AAudioBackend::get_buffer (PortEngine::PortHandle port_handle, pframes_t nframes)
{
	BackendPortPtr port = std::dynamic_pointer_cast<BackendPort> (port_handle);
	assert (port);
	assert (valid_port (port));
	return port->get_buffer (nframes);
}

int
AAudioBackend::register_system_ports ()
{
	LatencyRange lr;

	/* capture ports: an input of the engine is an output of the device */
	lr.min = lr.max = _systemic_input_latency;

	for (uint32_t i = 1; i <= _n_inputs; ++i) {
		char tmp[64];
		snprintf (tmp, sizeof (tmp), "system:capture_%u", i);
		BackendPortPtr p = add_port (std::string (tmp), DataType::AUDIO,
				static_cast<PortFlags> (IsOutput | IsPhysical | IsTerminal));
		if (!p) {
			return -1;
		}
		set_latency_range (p, false, lr);
		_system_inputs.push_back (p);
	}

	/* playback ports: an output of the engine is an input of the device */
	lr.min = lr.max = _systemic_output_latency;

	for (uint32_t i = 1; i <= _n_outputs; ++i) {
		char tmp[64];
		snprintf (tmp, sizeof (tmp), "system:playback_%u", i);
		BackendPortPtr p = add_port (std::string (tmp), DataType::AUDIO,
				static_cast<PortFlags> (IsInput | IsPhysical | IsTerminal));
		if (!p) {
			return -1;
		}
		set_latency_range (p, true, lr);
		_system_outputs.push_back (p);
	}

	return 0;
}

BackendPort*
AAudioBackend::port_factory (std::string const & name, ARDOUR::DataType type, ARDOUR::PortFlags flags)
{
	BackendPort* port = 0;

	switch (type) {
	case DataType::AUDIO:
		port = new AAudioAudioPort (*this, name, flags);
		break;
	default:
		PBD::error << _("AAudioBackend: unsupported port type.") << endmsg;
		return 0;
	}

	return port;
}

void
AAudioBackend::close_devices ()
{
	if (_output_device) {
		aa_device_stop (_output_device);
		aa_device_close (_output_device);
		_output_device = 0;
	}
	if (_input_device) {
		aa_device_stop (_input_device);
		aa_device_close (_input_device);
		_input_device = 0;
	}
}

int
AAudioBackend::_start (bool /*for_latency_measurement*/)
{
	if (_running) {
		PBD::error << _("AAudioBackend: already active.") << endmsg;
		return BackendReinitializationError;
	}

	clear_ports ();

	if (register_system_ports ()) {
		PBD::error << _("AAudioBackend: failed to register system ports.") << endmsg;
		return PortRegistrationError;
	}

	engine.sample_rate_change (_samplerate);
	engine.buffer_size_change (_samples_per_period);

	if (engine.reestablish_ports ()) {
		PBD::error << _("AAudioBackend: failed to reestablish ports.") << endmsg;
		stop ();
		return PortReconnectError;
	}
	engine.reconnect_ports ();

	_output_device = aa_device_open ((int32_t) _samplerate, (int32_t) _n_outputs, (int32_t) _samples_per_period);

	if (_output_device == 0) {
		PBD::error << _("AAudioBackend: out of memory.") << endmsg;
		return AudioDeviceOpenError;
	}

	if (aa_device_get_sample_rate (_output_device) == 0) {
		PBD::error << _("AAudioBackend: could not open the output device: ")
			   << aa_device_get_last_error (_output_device) << endmsg;
		close_devices ();
		return AudioDeviceOpenError;
	}

	if (_n_inputs > 0) {
		_input_device = aa_device_open_input ((int32_t) _samplerate, (int32_t) _n_inputs, (int32_t) _samples_per_period);
		if (_input_device && aa_device_get_sample_rate (_input_device) == 0) {
			PBD::warning << _("AAudioBackend: could not open the input device: ")
				     << aa_device_get_last_error (_input_device) << endmsg;
			aa_device_close (_input_device);
			_input_device = 0;
		}
	}

	if (aa_device_start (_output_device) != AAUDIO_DEVICE_OK) {
		PBD::error << _("AAudioBackend: could not start the output device: ")
			   << aa_device_get_last_error (_output_device) << endmsg;
		close_devices ();
		return AudioDeviceOpenError;
	}

	if (_input_device && aa_device_start (_input_device) != AAUDIO_DEVICE_OK) {
		PBD::warning << _("AAudioBackend: could not start the input device: ")
			     << aa_device_get_last_error (_input_device) << endmsg;
		aa_device_close (_input_device);
		_input_device = 0;
	}

	bool ok = _realtime;

	if (ok && pbd_realtime_pthread_create ("AAudio Main", PBD_SCHED_FIFO, PBD_RT_PRI_MAIN,
			PBD_RT_STACKSIZE_PROC, &_main_thread, aaudio_process_thread, this)) {
		PBD::warning << _("AAudioBackend: failed to acquire realtime permissions.") << endmsg;
		ok = false;
	}

	if (!ok && pbd_pthread_create (PBD_RT_STACKSIZE_PROC, &_main_thread, aaudio_process_thread, this)) {
		PBD::error << _("AAudioBackend: cannot create process thread.") << endmsg;
		close_devices ();
		return ProcessThreadStartError;
	}

	int timeout = 5000;
	while (!_running && --timeout > 0) {
		Glib::usleep (1000);
	}

	if (timeout == 0 || !_running) {
		PBD::error << _("AAudioBackend: the process thread did not start.") << endmsg;
		close_devices ();
		return ProcessThreadStartError;
	}

	return NoError;
}

int
AAudioBackend::stop ()
{
	void* status;

	if (_running) {
		_running = false;
		pthread_join (_main_thread, &status);
	}

	close_devices ();
	unregister_ports ();

	return 0;
}

void
AAudioBackend::capture_input (pframes_t nframes)
{
	if (!_input_device || _system_inputs.empty ()) {
		return;
	}

	const uint32_t channels = _n_inputs;
	const int64_t timeout = (int64_t) (nframes / _samplerate * 1000000000.0) * 2;

	int32_t got = aa_device_read (_input_device, _input_buffer.data (), (int32_t) nframes, timeout);

	if (got < 0) {
		PBD::error << _("AAudioBackend: error reading the input device: ")
			   << aa_device_get_last_error (_input_device) << endmsg;
		_running = false;
		return;
	}

	if (got < (int32_t) nframes) {
		/* not enough data: pad with silence, the engine will underrun */
		memset (_input_buffer.data () + got * channels, 0,
			sizeof (float) * ((nframes - got) * channels));
	}

	/* the device data is interleaved, the port buffers are not */
	for (uint32_t c = 0; c < channels && c < _system_inputs.size (); ++c) {
		/* a capture port is an output of the device side: write into it */
		std::shared_ptr<AAudioAudioPort> port = std::dynamic_pointer_cast<AAudioAudioPort> (_system_inputs[c]);
		if (!port) {
			continue;
		}
		Sample* buf = port->buffer ();
		for (pframes_t f = 0; f < nframes; ++f) {
			buf[f] = _input_buffer[f * channels + c];
		}
	}
}

void
AAudioBackend::render_output (pframes_t nframes)
{
	if (!_output_device || _system_outputs.empty ()) {
		return;
	}

	const uint32_t channels = _n_outputs;

	memset (_output_buffer.data (), 0, sizeof (float) * nframes * channels);

	for (uint32_t c = 0; c < channels && c < _system_outputs.size (); ++c) {
		/* a playback port is an input: its buffer is the mix of its connections */
		Sample* buf = (Sample*) _system_outputs[c]->get_buffer (nframes);
		if (!buf) {
			continue;
		}
		for (pframes_t f = 0; f < nframes; ++f) {
			_output_buffer[f * channels + c] = buf[f];
		}
	}

	const int64_t timeout = (int64_t) (nframes / _samplerate * 1000000000.0) * 2;

	int32_t written = aa_device_write (_output_device, _output_buffer.data (), (int32_t) nframes, timeout);

	if (written < 0) {
		PBD::error << _("AAudioBackend: error writing to the output device: ")
			   << aa_device_get_last_error (_output_device) << endmsg;
		_running = false;
	}
}

void*
AAudioBackend::aaudio_process_thread (void *arg)
{
	return reinterpret_cast<AAudioBackend*> (arg)->main_process_thread ();
}

void*
AAudioBackend::main_process_thread ()
{
	AudioEngine::thread_init_callback (this);

	_running = true;
	_processed_samples = 0;

	manager.registration_callback ();
	manager.graph_order_callback ();

	const pframes_t nframes = _samples_per_period;
	const uint32_t out_channels = _n_outputs > 0 ? _n_outputs : 1;
	const uint32_t in_channels = _n_inputs;

	_output_buffer.assign (nframes * out_channels, 0.0f);
	if (in_channels > 0) {
		_input_buffer.assign (nframes * in_channels, 0.0f);
	}

	while (_running) {

		if (_freewheeling != _freewheel) {
			_freewheel = _freewheeling;
			engine.freewheel_callback (_freewheel);
		}

		if (_input_device) {
			capture_input (nframes);
		}

		_dsp_load_calc.set_max_time (_samplerate, nframes);
		_dsp_load_calc.set_start_timestamp_us (_x_get_monotonic_usec ());

		if (engine.process_callback (nframes)) {
			break;
		}

		_processed_samples += nframes;

		if (_output_device) {
			render_output (nframes);
		}

		_dsp_load_calc.set_stop_timestamp_us (_x_get_monotonic_usec ());
		_dsp_load = _dsp_load_calc.get_dsp_load_unbound ();
	}

	_running = false;

	return 0;
}

/* ---------------------------------------------------------------------- */
/* module export                                                           */
/* ---------------------------------------------------------------------- */

static std::shared_ptr<AAudioBackend> _instance;

static std::shared_ptr<AudioBackend> backend_factory (AudioEngine& e);

static int
instantiate (const std::string& arg1, const std::string& /* arg2 */)
{
	s_instance_name = arg1;
	return 0;
}

static int
deinstantiate ()
{
	_instance.reset ();
	return 0;
}

static bool
already_configured ()
{
	return false;
}

static bool
available ()
{
	/* the AAudio API only exists on Android */
#ifdef PLATFORM_ANDROID
	return true;
#else
	return false;
#endif
}

static ARDOUR::AudioBackendInfo _descriptor = {
	_("AAudio"), instantiate, deinstantiate, backend_factory, already_configured, available
};

static std::shared_ptr<AudioBackend>
backend_factory (AudioEngine& e)
{
	if (!_instance) {
		_instance.reset (new AAudioBackend (e, _descriptor));
	}
	return _instance;
}

extern "C" ARDOURBACKEND_API ARDOUR::AudioBackendInfo* descriptor ()
{
	return &_descriptor;
}
