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

    fun run(host: EngineHost) {
        Thread {
            val result = try {
                check(host)
                "ARDOUR_SMOKE PASS"
            } catch (t: Throwable) {
                "ARDOUR_SMOKE FAIL ${t.message}"
            }
            Log.i(TAG, result)
        }.start()
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
