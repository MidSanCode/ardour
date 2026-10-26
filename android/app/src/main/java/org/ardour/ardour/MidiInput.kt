package org.ardour.ardour

import android.content.Context
import android.content.pm.PackageManager
import android.media.midi.MidiDevice
import android.media.midi.MidiDeviceInfo
import android.media.midi.MidiManager
import android.media.midi.MidiReceiver
import android.os.Handler
import android.os.Looper
import android.util.Log
import org.json.JSONObject
import java.util.concurrent.Executors

/**
 * MIDI from the devices attached to the phone (USB, Bluetooth, virtual):
 * every output port of every device is opened, and what it sends goes to the
 * engine's MIDI input (midi.send), which the MIDI tracks listen to.  Devices
 * that come and go while the app runs are followed.
 */
class MidiInput(private val context: Context, private val host: EngineHost) {
    private val manager = context.getSystemService(Context.MIDI_SERVICE) as MidiManager?
    private val handler = Handler(Looper.getMainLooper())
    private val devices = HashMap<Int, MidiDevice>()
    /* one sender, so that messages keep their order and the MIDI thread never waits */
    private val sender = Executors.newSingleThreadExecutor()

    private val receiver = object : MidiReceiver() {
        override fun onSend(msg: ByteArray, offset: Int, count: Int, timestamp: Long) {
            val hex = StringBuilder(count * 3)
            for (i in offset until offset + count) hex.append(String.format("%02x ", msg[i].toInt() and 0xff))
            val params = JSONObject().put("data", hex.toString().trim()).toString()
            sender.execute { host.call("midi.send", params) }
        }
    }

    private val callback = object : MidiManager.DeviceCallback() {
        override fun onDeviceAdded(device: MidiDeviceInfo) = open(device)
        override fun onDeviceRemoved(device: MidiDeviceInfo) {
            devices.remove(device.id)?.close()
            Log.i(TAG, "MIDI device removed: ${name(device)}")
        }
    }

    fun start() {
        if (manager == null || !context.packageManager.hasSystemFeature(PackageManager.FEATURE_MIDI)) {
            Log.i(TAG, "no MIDI support on this device")
            return
        }
        @Suppress("DEPRECATION")
        manager.devices.forEach { open(it) }
        manager.registerDeviceCallback(callback, handler)
    }

    fun stop() {
        manager?.unregisterDeviceCallback(callback)
        devices.values.forEach { it.close() }
        devices.clear()
    }

    private fun name(info: MidiDeviceInfo) =
        info.properties.getString(MidiDeviceInfo.PROPERTY_NAME) ?: "device ${info.id}"

    private fun open(info: MidiDeviceInfo) {
        if (info.outputPortCount == 0 || devices.containsKey(info.id)) return
        manager?.openDevice(info, { device ->
            if (device == null) {
                Log.w(TAG, "could not open MIDI device ${name(info)}")
                return@openDevice
            }
            devices[info.id] = device
            for (port in info.ports) {
                if (port.type == MidiDeviceInfo.PortInfo.TYPE_OUTPUT) {
                    device.openOutputPort(port.portNumber)?.connect(receiver)
                }
            }
            Log.i(TAG, "MIDI device opened: ${name(info)}")
        }, handler)
    }

    companion object {
        private const val TAG = "ArdourMidi"
    }
}
