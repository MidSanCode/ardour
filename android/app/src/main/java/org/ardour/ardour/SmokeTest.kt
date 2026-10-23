package org.ardour.ardour

import android.util.Log
import org.json.JSONObject

/**
 * The check CI runs on an emulator (am start ... --ez smoketest true): it
 * drives the engine through the same protocol as the frontend and writes one
 * line, "ARDOUR_SMOKE PASS" or "ARDOUR_SMOKE FAIL <reason>", to the log.
 */
object SmokeTest {

    private const val TAG = "ArdourSmoke"

    fun run(host: EngineHost, done: (Boolean) -> Unit = {}) {
        Thread {
            var passed = false
            val result = try {
                check(host)
                checkEditing(host)
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
    private fun checkEditing(host: EngineHost) {
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
