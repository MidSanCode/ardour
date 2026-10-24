package org.ardour.ardour

import android.util.Log
import org.json.JSONObject
import java.io.File
import java.io.FileOutputStream
import java.nio.ByteBuffer
import java.nio.ByteOrder

/**
 * The check CI runs on an emulator (am start ... --ez smoketest true): it
 * drives the engine through the same protocol as the frontend and writes one
 * line, "ARDOUR_SMOKE PASS" or "ARDOUR_SMOKE FAIL <reason>", to the log.
 */
object SmokeTest {

    private const val TAG = "ArdourSmoke"

    fun run(host: EngineHost, scratch: File, done: (Boolean) -> Unit = {}) {
        Thread {
            var passed = false
            val result = try {
                check(host)
                checkEditing(host, scratch)
                passed = true
                "ARDOUR_SMOKE PASS"
            } catch (t: Throwable) {
                "ARDOUR_SMOKE FAIL ${t.message}"
            }
            Log.i(TAG, result)
            done(passed)
        }.start()
    }

    private fun fail(why: String): Nothing = throw IllegalStateException(why)

    private fun near(a: Double, b: Double, tolerance: Double) = Math.abs(a - b) <= tolerance

    private fun trackRegions(host: EngineHost, trackId: String): org.json.JSONArray {
        val tracks = call(host, "timeline").getJSONArray("tracks")
        for (i in 0 until tracks.length()) {
            val t = tracks.getJSONObject(i)
            if (t.getString("id") == trackId) return t.getJSONArray("regions")
        }
        fail("track $trackId is not on the timeline")
    }

    /**
     * The mixer and the editor timeline: busses, strip controls, regions made
     * from a test tone, moving, splitting, muting and removing them, and a
     * meter reading while the tone plays.  The session is left loaded so the
     * frontend has something to show.
     */
    /** a mono 16-bit WAV file of a sine tone */
    private fun writeWav(file: File, rate: Int, seconds: Double, frequency: Double) {
        val frames = (rate * seconds).toInt()
        val data = ByteBuffer.allocate(44 + frames * 2).order(ByteOrder.LITTLE_ENDIAN)
        data.put("RIFF".toByteArray()).putInt(36 + frames * 2).put("WAVE".toByteArray())
        data.put("fmt ".toByteArray()).putInt(16).putShort(1).putShort(1).putInt(rate).putInt(rate * 2).putShort(2).putShort(16)
        data.put("data".toByteArray()).putInt(frames * 2)
        for (i in 0 until frames) {
            data.putShort((Math.sin(2 * Math.PI * frequency * i / rate) * 0.4 * 32767).toInt().toShort())
        }
        FileOutputStream(file).use { it.write(data.array()) }
    }

    private fun regionById(host: EngineHost, trackId: String, id: String): JSONObject {
        val regions = trackRegions(host, trackId)
        for (i in 0 until regions.length()) if (regions.getJSONObject(i).getString("id") == id) return regions.getJSONObject(i)
        fail("region $id is not on track $trackId")
    }

    /** importing a file, trimming, fades, undo and redo, and recording */
    private fun checkImportAndHistory(host: EngineHost, scratch: File, rate: Long, vocals: String) {
        val wav = File(scratch, "smoke-import.wav")
        writeWav(wav, rate.toInt(), 1.0, 220.0)
        val imported = call(host, "regions.import",
            JSONObject().put("track", vocals).put("path", wav.absolutePath).put("position", 5 * rate)).getJSONObject("region")
        val id = imported.getString("id")
        if (imported.getLong("length") != rate) fail("the imported region has the wrong length: $imported")

        val trimmed = call(host, "region.trim",
            JSONObject().put("id", id).put("start", 5 * rate + rate / 4).put("end", 5 * rate + 3 * rate / 4)).getJSONObject("region")
        if (trimmed.getLong("position") != 5 * rate + rate / 4) fail("the front trim did not apply: $trimmed")
        if (!near(trimmed.getLong("length").toDouble(), rate / 2.0, 2.0)) fail("the end trim did not apply: $trimmed")
        if (trimmed.getLong("start") != rate / 4) fail("the front trim did not keep the audio in place: $trimmed")

        val faded = call(host, "region.fades", JSONObject().put("id", id).put("fade_in", 2400).put("fade_out", 4800))
            .getJSONObject("region")
        if (faded.getLong("fade_in") != 2400L || faded.getLong("fade_out") != 4800L) fail("the fades did not apply: $faded")

        val undone = call(host, "edit.undo")
        if (undone.optString("redo") != "change fades") fail("undo did not undo the fades: $undone")
        if (regionById(host, vocals, id).getLong("fade_in") == 2400L) fail("the fade is still there after undo")
        call(host, "edit.undo")
        if (regionById(host, vocals, id).getLong("length") != rate) fail("undo did not restore the trimmed region")
        call(host, "edit.redo")
        call(host, "edit.redo")
        val again = regionById(host, vocals, id)
        if (again.getLong("fade_in") != 2400L || again.getLong("position") != 5 * rate + rate / 4) fail("redo did not restore the edits: $again")

        /* recording: the emulator's input may be silent or missing, so this
         * only requires the protocol to work and reports what was captured */
        val before = trackRegions(host, vocals).length()
        call(host, "strip.set", JSONObject().put("id", vocals).put("rec", true))
        call(host, "transport.locate", JSONObject().put("position", 8 * rate))
        Thread.sleep(200)
        val armed = call(host, "transport.record", JSONObject().put("enabled", true))
        if (!armed.optBoolean("record_enabled")) fail("the session did not arm for recording: $armed")
        call(host, "transport.play")
        var recording = false
        for (i in 0 until 20) {
            Thread.sleep(100)
            if (call(host, "transport.state").optBoolean("recording")) recording = true
        }
        call(host, "transport.stop")
        Thread.sleep(1000)
        call(host, "transport.record", JSONObject().put("enabled", false))
        call(host, "strip.set", JSONObject().put("id", vocals).put("rec", false))
        val after = trackRegions(host, vocals).length()
        Log.i(TAG, "recording: rolled while recording=$recording, regions on the track $before -> $after")
    }

    private fun checkEditing(host: EngineHost, scratch: File) {
        val rate = call(host, "engine.status").optLong("sample_rate", 48000L)

        call(host, "session.create", JSONObject().put("name", "ui-" + System.currentTimeMillis()))
        val vocals = call(host, "tracks.add", JSONObject().put("name", "Vocals").put("channels", 1)).getString("id")
        val guitar = call(host, "tracks.add", JSONObject().put("name", "Guitar").put("channels", 2)).getString("id")
        val temp = call(host, "tracks.add", JSONObject().put("name", "Temp").put("channels", 1)).getString("id")
        call(host, "buses.add", JSONObject().put("name", "Reverb"))
        call(host, "tracks.remove", JSONObject().put("id", temp))

        val strips = call(host, "mixer.strips").getJSONArray("strips")
        if (strips.length() != 4) fail("expected 4 mixer strips, found ${strips.length()}")
        if (strips.getJSONObject(3).getString("kind") != "master") fail("the master bus is not the last strip")
        if (strips.getJSONObject(2).getString("kind") != "bus") fail("the bus strip is missing")

        var strip = call(host, "strip.set", JSONObject().put("id", vocals).put("gain_db", -6).put("pan", -0.5).put("mute", true))
            .getJSONObject("strip")
        if (!near(strip.getDouble("gain_db"), -6.0, 0.05)) fail("gain did not apply: $strip")
        if (!near(strip.getDouble("pan"), -0.5, 0.01)) fail("pan did not apply: $strip")
        if (!strip.getBoolean("mute")) fail("mute did not apply: $strip")
        strip = call(host, "strip.set", JSONObject().put("id", vocals).put("mute", false).put("gain_db", 0)).getJSONObject("strip")
        if (strip.getBoolean("mute")) fail("unmute did not apply")
        call(host, "strip.set", JSONObject().put("id", guitar).put("solo", true))
        call(host, "strip.set", JSONObject().put("id", guitar).put("solo", false))

        val first = call(host, "regions.tone", JSONObject().put("track", vocals).put("position", 0).put("seconds", 2))
            .getJSONObject("region")
        if (!near(first.getLong("length").toDouble(), 2.0 * rate, rate * 0.01)) fail("the tone region has the wrong length: $first")
        call(host, "regions.tone", JSONObject().put("track", guitar).put("position", 3 * rate).put("seconds", 2).put("frequency", 660))

        val peaks = call(host, "region.peaks", JSONObject().put("id", first.getString("id")).put("count", 50)).getJSONArray("peaks")
        var loudest = 0.0
        for (i in 0 until peaks.length()) loudest = Math.max(loudest, Math.abs(peaks.getDouble(i)))
        if (loudest < 0.3) fail("the tone region's waveform is silent ($loudest)")

        call(host, "region.move", JSONObject().put("id", first.getString("id")).put("position", rate))
        var regions = trackRegions(host, vocals)
        if (regions.length() != 1 || regions.getJSONObject(0).getLong("position") != rate) fail("the region did not move: $regions")

        call(host, "region.split", JSONObject().put("id", first.getString("id")).put("position", 2 * rate))
        regions = trackRegions(host, vocals)
        if (regions.length() != 2) fail("the split left ${regions.length()} regions")
        val left = regions.getJSONObject(0).getString("id")
        val right = regions.getJSONObject(1).getString("id")
        if (!call(host, "region.mute", JSONObject().put("id", left).put("muted", true)).getJSONObject("region").getBoolean("muted")) {
            fail("the region did not mute")
        }
        call(host, "region.remove", JSONObject().put("id", left))
        call(host, "region.move", JSONObject().put("id", right).put("track", guitar).put("position", 6 * rate))
        if (trackRegions(host, vocals).length() != 0) fail("the region did not leave its track")
        if (trackRegions(host, guitar).length() != 2) fail("the region did not arrive on the other track")

        checkImportAndHistory(host, scratch, rate, vocals)

        /* material for the frontend, and a signal for the meters */
        call(host, "regions.tone", JSONObject().put("track", vocals).put("position", 0).put("seconds", 4).put("frequency", 330))
        call(host, "transport.locate", JSONObject().put("position", 0))
        Thread.sleep(300)
        call(host, "transport.play")
        var heard = -200.0
        for (i in 0 until 40) {
            Thread.sleep(100)
            val meters = call(host, "meters").getJSONArray("meters")
            for (m in 0 until meters.length()) {
                val meter = meters.getJSONObject(m)
                if (meter.getString("id") != vocals) continue
                val peak = meter.getJSONArray("peak")
                for (c in 0 until peak.length()) heard = Math.max(heard, peak.getDouble(c))
            }
            if (heard > -30) break
        }
        call(host, "transport.stop")
        if (heard <= -30) fail("the tone did not reach the track meter (peak $heard dBFS)")
        Log.i(TAG, "track meter peak $heard dBFS while the tone played")
        Thread.sleep(300)
        call(host, "transport.locate", JSONObject().put("position", rate / 2))
        call(host, "session.save")
    }

    private fun call(host: EngineHost, method: String, params: JSONObject = JSONObject()): JSONObject {
        val reply = JSONObject(host.call(method, params.toString()))
        Log.i(TAG, "$method -> $reply")
        if (!reply.optBoolean("ok", false)) {
            throw IllegalStateException("$method: ${reply.optString("error")}")
        }
        return reply
    }

    private fun check(host: EngineHost) {
        host.start().get()

        val status = call(host, "engine.status")
        if (!status.optBoolean("running")) throw IllegalStateException("the engine is not running")
        Log.i(TAG, "backend ${status.optString("backend")} at ${status.optInt("sample_rate")} Hz")

        val name = "smoke-" + System.currentTimeMillis()
        call(host, "session.create", JSONObject().put("name", name))
        call(host, "tracks.add", JSONObject().put("name", "Audio").put("channels", 1))
        val tracks = call(host, "tracks.list").getJSONArray("tracks")
        if (tracks.length() != 1) throw IllegalStateException("expected 1 track, found ${tracks.length()}")

        call(host, "transport.play")
        var rolled = false
        for (i in 0 until 50) {
            Thread.sleep(100)
            val t = call(host, "transport.state")
            if (t.optBoolean("rolling") && t.optLong("position") > 0) {
                rolled = true
                break
            }
        }
        if (!rolled) throw IllegalStateException("the transport did not roll")
        call(host, "meters")
        call(host, "transport.stop")
        call(host, "session.save")
        call(host, "session.close")
        val list = call(host, "session.list").getJSONArray("sessions")
        var found = false
        for (i in 0 until list.length()) if (list.getString(i) == name) found = true
        if (!found) throw IllegalStateException("the saved session is not listed")
    }
}
