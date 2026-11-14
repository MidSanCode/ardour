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

    /**
     * What the audio path actually did, for the CI to check from the log.
     *
     * The emulator runs with -noaudio, so the AAudio backend cannot open a
     * stream there and the engine falls back to "None (Dummy)".  That is a
     * fallback, not a failure, and the point of this line is to make the
     * difference visible instead of hiding it: the CI job asserts the
     * fallback explicitly, and the same line on a device says "realtime".
     *
     * Every field comes from engine.status / transport.state; nothing is
     * guessed.  `advanced` is whether the transport position moved across a
     * short sleep, which is the proof that the process cycle is really being
     * called back (through AAudio on a device, through the dummy thread on an
     * emulator), and `dsp` is the load that cycle reported.
     */
    private fun logAudioPath(host: EngineHost, where: String) {
        val status = call(host, "engine.status")
        val backend = status.optString("backend")
        val rate = status.optLong("sample_rate", 0)
        val buffer = status.optLong("buffer_size", 0)
        val dsp = status.optDouble("dsp_load", 0.0)
        val live = rate > 0 && buffer > 0

        /* position must advance while rolling: that is the proof the cycle
         * callback runs, whether through AAudio or the dummy thread */
        val before = call(host, "transport.state").optLong("position", -1)
        Thread.sleep(250)
        val after = call(host, "transport.state").optLong("position", -1)
        val advanced = before >= 0 && after > before

        Log.i(TAG, "ARDOUR_AUDIO $where backend=\"$backend\" rate=$rate buffer=$buffer " +
                   "dsp=${"%.4f".format(dsp)} live=$live advanced=$advanced")
    }

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

        /* a file at another rate is resampled to the session's */
        val other = if (rate == 22050L) 44100 else 22050
        val wav2 = File(scratch, "smoke-import-$other.wav")
        writeWav(wav2, other, 1.0, 440.0)
        val resampled = call(host, "regions.import",
            JSONObject().put("track", vocals).put("path", wav2.absolutePath).put("position", 12 * rate))
        val rlen = resampled.getJSONObject("region").getLong("length")
        if (resampled.optInt("resampled_from") != other || !near(rlen.toDouble(), rate.toDouble(), rate * 0.01))
            fail("the $other Hz file was not resampled to $rate Hz: $resampled")
        Log.i(TAG, "imported a $other Hz file as $rlen samples at $rate Hz")
        call(host, "region.remove", JSONObject().put("id", resampled.getJSONObject("region").getString("id")))

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

    private fun peakOf(host: EngineHost, id: String): Double {
        var heard = -200.0
        val meters = call(host, "meters").getJSONArray("meters")
        for (m in 0 until meters.length()) {
            val meter = meters.getJSONObject(m)
            if (meter.getString("id") != id) continue
            val peak = meter.getJSONArray("peak")
            for (c in 0 until peak.length()) heard = Math.max(heard, peak.getDouble(c))
        }
        return heard
    }

    private fun stripOf(host: EngineHost, id: String): JSONObject {
        val strips = call(host, "mixer.strips").getJSONArray("strips")
        for (i in 0 until strips.length()) if (strips.getJSONObject(i).getString("id") == id) return strips.getJSONObject(i)
        fail("no mixer strip for $id")
    }

    private fun noteCount(host: EngineHost, region: String) =
        call(host, "region.notes", JSONObject().put("id", region)).getJSONArray("notes").length()

    /** a MIDI track with an instrument, a MIDI region and its notes, and an effect plugin */
    private fun checkMidiAndPlugins(host: EngineHost, rate: Long, audioTrack: String): String {
        val plugins = call(host, "plugins.list").getJSONArray("plugins")
        var instrument: JSONObject? = null
        var effect: JSONObject? = null
        for (i in 0 until plugins.length()) {
            val p = plugins.getJSONObject(i)
            if (p.getBoolean("instrument") && p.getString("name").contains("Reasonable")) instrument = p
            /* the stereo variant, for the stereo track it goes on */
            if (!p.getBoolean("instrument") && p.getString("type") == "lv2" && p.getString("name").contains("Compressor") &&
                (effect == null || p.getString("name").lowercase().contains("stereo"))) effect = p
        }
        Log.i(TAG, "plugins: ${plugins.length()} available")
        if (instrument == null) fail("ACE Reasonable Synth is not among the plugins: $plugins")
        if (effect == null) fail("ACE Compressor is not among the plugins")

        val keys = call(host, "tracks.add", JSONObject().put("kind", "midi").put("name", "Keys")
            .put("instrument", instrument.getString("id"))).getString("id")
        val keysPlugins = stripOf(host, keys).getJSONArray("plugins")
        if (keysPlugins.length() != 1 || !keysPlugins.getJSONObject(0).getBoolean("instrument"))
            fail("the MIDI track does not have its instrument: $keysPlugins")

        val region = call(host, "regions.midi", JSONObject().put("track", keys).put("position", 0).put("beats", 8))
            .getJSONObject("region")
        val rid = region.getString("id")
        if (region.optString("kind") != "midi" || !near(region.getDouble("length_beats"), 8.0, 0.01))
            fail("the MIDI region is wrong: $region")
        val ids = ArrayList<Long>()
        for ((n, beat) in listOf(60 to 0.0, 64 to 1.0, 67 to 2.0, 72 to 3.0)) {
            ids.add(call(host, "region.note.add", JSONObject().put("id", rid).put("note", n).put("start", beat)
                .put("length", 0.9).put("velocity", 110)).getLong("id"))
        }
        if (noteCount(host, rid) != 4) fail("the notes were not added")
        call(host, "region.note.edit", JSONObject().put("id", rid).put("note_id", ids[3]).put("note", 71).put("length", 2))
        val edited = call(host, "region.notes", JSONObject().put("id", rid)).getJSONArray("notes")
        var found = false
        for (i in 0 until edited.length()) {
            val n = edited.getJSONObject(i)
            if (n.getInt("note") == 71 && near(n.getDouble("length"), 2.0, 0.01)) found = true
        }
        if (!found) fail("the note edit did not apply: $edited")
        call(host, "region.note.remove", JSONObject().put("id", rid).put("note_id", ids[1]))
        if (noteCount(host, rid) != 3) fail("the note was not removed")
        call(host, "edit.undo")
        if (noteCount(host, rid) != 4) fail("undo did not bring the note back")
        call(host, "edit.redo")
        if (noteCount(host, rid) != 3) fail("redo did not remove the note again")

        /* the instrument plays the notes */
        call(host, "transport.locate", JSONObject().put("position", 0))
        Thread.sleep(300)
        call(host, "transport.play")
        var heard = -200.0
        for (i in 0 until 40) {
            Thread.sleep(100)
            heard = Math.max(heard, peakOf(host, keys))
            if (heard > -40) break
        }
        call(host, "transport.stop")
        Thread.sleep(500)
        if (heard <= -40) fail("the synth did not sound (peak $heard dBFS)")
        Log.i(TAG, "synth peak $heard dBFS while the MIDI region played")

        /* Recording from the MIDI input (the on-screen keyboard's path).
         *
         * The notes reach the track through the session's virtual keyboard
         * port and are picked up by the process cycle, so a loaded emulator
         * can drop one of them - API 28 is slow enough for that to happen.
         * What must not happen is the path stopping to work, so a short
         * result is retried once before it is called a failure. */
        fun recordOnce(): Int {
            val regionsBefore = trackRegions(host, keys).length()
            call(host, "strip.set", JSONObject().put("id", keys).put("rec", true))
            call(host, "transport.locate", JSONObject().put("position", 10 * rate))
            Thread.sleep(200)
            call(host, "transport.record", JSONObject().put("enabled", true))
            call(host, "transport.play")
            /* the transport has to be rolling before a note is worth sending:
             * on a slow emulator it can take a second to start, and notes sent
             * before that are not recorded at all */
            var rolling = false
            for (i in 0 until 40) {
                Thread.sleep(100)
                if (call(host, "transport.state").optBoolean("rolling")) { rolling = true; break }
            }
            if (!rolling) fail("the transport did not roll for the MIDI recording")
            Thread.sleep(300)
            for (n in listOf(60, 62, 64)) {
                call(host, "midi.send", JSONObject().put("track", keys).put("note", n).put("velocity", 90).put("on", true))
                Thread.sleep(150)
                call(host, "midi.send", JSONObject().put("track", keys).put("note", n).put("on", false))
                Thread.sleep(100)
            }
            /* raw bytes too, as a MIDI device sends them, with running status */
            call(host, "midi.send", JSONObject().put("track", keys).put("data", "90 43 50 47 50"))
            Thread.sleep(150)
            call(host, "midi.send", JSONObject().put("track", keys).put("data", "80 43 00 47 00"))
            Thread.sleep(300)
            call(host, "transport.stop")
            Thread.sleep(1500)
            call(host, "transport.record", JSONObject().put("enabled", false))
            call(host, "strip.set", JSONObject().put("id", keys).put("rec", false))
            val keyRegions = trackRegions(host, keys)
            if (keyRegions.length() <= regionsBefore) fail("no MIDI region was recorded")
            var notes = 0
            for (i in 0 until keyRegions.length()) {
                val r = keyRegions.getJSONObject(i)
                if (r.getString("id") == rid) continue
                notes = Math.max(notes, noteCount(host, r.getString("id")))
            }
            return notes
        }

        var recorded = recordOnce()
        if (recorded < 5) {
            Log.i(TAG, "only $recorded of 5 notes arrived, recording once more")
            recorded = Math.max(recorded, recordOnce())
        }
        if (recorded < 5) fail("the recorded MIDI region has $recorded notes, expected 5")
        Log.i(TAG, "recorded $recorded MIDI notes from the input")

        /* an effect: add, change a parameter, bypass, remove */
        val added = call(host, "plugin.add", JSONObject().put("route", audioTrack).put("id", effect.getString("id")))
            .getJSONObject("plugin")
        val pid = added.getString("id")
        val params = added.getJSONArray("params")
        if (params.length() == 0) fail("the effect shows no parameters")
        var param: JSONObject? = null
        for (i in 0 until params.length()) {
            val pp = params.getJSONObject(i)
            if (!pp.getBoolean("toggled") && !pp.getBoolean("integer") && pp.getDouble("max") > pp.getDouble("min")) { param = pp; break }
        }
        if (param == null) fail("the effect has no continuous parameter: $params")
        val target = param.getDouble("min") + (param.getDouble("max") - param.getDouble("min")) * 0.75
        val set = call(host, "plugin.set", JSONObject().put("route", audioTrack).put("plugin", pid)
            .put("param", param.getInt("index")).put("value", target)).getJSONObject("plugin").getJSONArray("params")
        var value = Double.NaN
        for (i in 0 until set.length()) if (set.getJSONObject(i).getInt("index") == param.getInt("index")) value = set.getJSONObject(i).getDouble("value")
        if (!near(value, target, Math.abs(target) * 0.01 + 1e-3)) fail("the parameter did not change: $value, wanted $target")
        val bypassed = call(host, "plugin.set", JSONObject().put("route", audioTrack).put("plugin", pid).put("active", false))
        if (bypassed.getJSONObject("plugin").getBoolean("active")) fail("the effect was not bypassed")
        call(host, "plugin.set", JSONObject().put("route", audioTrack).put("plugin", pid).put("active", true))
        if (stripOf(host, audioTrack).getJSONArray("plugins").length() != 1) fail("the effect is not on the strip")

        /* presets: the bundled ones load, a new one saves */
        val presets = call(host, "plugin.presets", JSONObject().put("route", audioTrack).put("plugin", pid)).getJSONArray("presets")
        if (presets.length() > 0) {
            val uri = presets.getJSONObject(0).getString("uri")
            call(host, "plugin.preset.load", JSONObject().put("route", audioTrack).put("plugin", pid).put("uri", uri))
            val cur = call(host, "plugin.presets", JSONObject().put("route", audioTrack).put("plugin", pid)).getString("current")
            if (cur != uri) fail("the preset $uri is not the current one ($cur)")
        }
        val saved = call(host, "plugin.preset.save", JSONObject().put("route", audioTrack).put("plugin", pid).put("name", "Smoke"))
            .getJSONArray("presets")
        var haveSaved = false
        for (i in 0 until saved.length()) if (saved.getJSONObject(i).getString("label") == "Smoke") haveSaved = true
        if (!haveSaved) fail("the saved preset is not listed: $saved")
        Log.i(TAG, "presets: ${presets.length()} bundled, saving one works")
        Log.i(TAG, "effect ${added.getString("name")}: ${params.length()} parameters, ${param.getString("name")} -> $value")
        return pid
    }

    /** gain automation: points, play mode following them, undo */
    private fun checkAutomation(host: EngineHost, rate: Long, track: String) {
        val base = JSONObject().put("route", track).put("control", "gain")
        fun with(vararg kv: Pair<String, Any>): JSONObject {
            val o = JSONObject(base.toString())
            for ((k, v) in kv) o.put(k, v)
            return o
        }
        call(host, "automation.add", with("t" to 0, "value" to 1.0))
        call(host, "automation.add", with("t" to 2 * rate, "value" to 0.1))
        call(host, "automation.add", with("t" to 4 * rate, "value" to 0.1))
        val extra = call(host, "automation.add", with("t" to 5 * rate, "value" to 0.5))
        if (extra.getJSONArray("points").length() != 4) fail("automation points were not added: $extra")
        val removed = call(host, "automation.remove", with("index" to 3))
        if (removed.getJSONArray("points").length() != 3) fail("the automation point was not removed")
        call(host, "edit.undo")
        if (call(host, "automation.get", base).getJSONArray("points").length() != 4) fail("undo did not restore the point")
        call(host, "edit.redo")

        call(host, "automation.state", with("state" to "play"))
        call(host, "transport.locate", JSONObject().put("position", 3 * rate))
        Thread.sleep(200)
        call(host, "transport.play")
        Thread.sleep(400)
        val gain = stripOf(host, track).getDouble("gain_db")
        call(host, "transport.stop")
        Thread.sleep(300)
        if (!near(gain, -20.0, 1.0)) fail("gain did not follow the automation: $gain dB")
        call(host, "automation.state", with("state" to "manual"))
        call(host, "strip.set", JSONObject().put("id", track).put("gain_db", 0))
        Log.i(TAG, "automation: gain followed the points to $gain dB")
    }

    /**
     * Export: the presets the engine finds, and a real render of the first two
     * seconds of the master bus.  The emulator has no audio device, so what is
     * checked is the file itself: a WAV with the CD preset's rate and width,
     * the length that was asked for, and audio in it rather than silence.
     */
    private fun checkExport(host: EngineHost, scratch: File, rate: Long) {
        val presets = call(host, "export.presets").getJSONArray("presets")
        if (presets.length() == 0) fail("the engine found no export presets")
        var cd: String? = null
        val names = StringBuilder()
        for (i in 0 until presets.length()) {
            val p = presets.getJSONObject(i)
            names.append(if (i == 0) "" else ", ").append(p.getString("name"))
            if (cd == null && p.getString("name").contains("CD")) cd = p.getString("id")
        }
        if (cd == null) fail("there is no CD (44.1 kHz WAV) export preset: $names")
        Log.i(TAG, "export presets: $names")

        val folder = File(scratch, "export")
        val params = JSONObject().put("folder", folder.absolutePath)
            .put("start", 0).put("end", 2 * rate).put("name", "smoke").put("preset", cd)
        val reply = call(host, "export.audio", params)
        val files = reply.getJSONArray("files")
        if (files.length() == 0) fail("the export wrote no file: $reply")
        /* the CD preset writes a WAV, but a preset may write several files
         * (cue sheets, several formats), so pick the audio one */
        var file: File? = null
        for (i in 0 until files.length()) {
            val f = File(files.getJSONObject(i).getString("path"))
            if (f.name.endsWith(".wav", ignoreCase = true)) { file = f; break }
            if (file == null || f.length() > file.length()) file = f
        }
        if (file == null || !file.isFile) fail("the exported file is not there: $reply")

        /* walk the RIFF chunks rather than assuming a 44 byte header */
        val bytes = file.readBytes()
        if (bytes.size < 44 || String(bytes, 0, 4) != "RIFF" || String(bytes, 8, 4) != "WAVE") {
            fail("the export is not a WAV file: ${file.name}")
        }
        val bb = ByteBuffer.wrap(bytes).order(ByteOrder.LITTLE_ENDIAN)
        var channels = 0
        var sampleRate = 0
        var bits = 0
        var dataStart = -1
        var dataLength = 0
        var pos = 12
        while (pos + 8 <= bytes.size) {
            val id = String(bytes, pos, 4)
            val size = bb.getInt(pos + 4)
            when (id) {
                "fmt " -> {
                    channels = bb.getShort(pos + 10).toInt()
                    sampleRate = bb.getInt(pos + 12)
                    bits = bb.getShort(pos + 22).toInt()
                }
                "data" -> {
                    dataStart = pos + 8
                    dataLength = Math.min(size, bytes.size - dataStart)
                }
            }
            pos += 8 + size + (size and 1)
        }
        if (channels < 1 || sampleRate < 8000 || bits != 16 || dataStart < 0) {
            fail("unexpected WAV format: ${channels} ch, $sampleRate Hz, $bits bit")
        }

        var peak = 0
        val frames = dataLength / (2 * channels)
        for (i in 0 until frames * channels) {
            val v = Math.abs(bb.getShort(dataStart + i * 2).toInt())
            if (v > peak) peak = v
        }
        val seconds = frames.toDouble() / sampleRate
        if (!near(seconds, 2.0, 0.05)) fail("the export is $seconds s long, expected 2")
        if (peak < 1000) fail("the export is silent (peak $peak of 32767)")

        Log.i(TAG, "ARDOUR_EXPORT file=${file.name} bytes=${file.length()} rate=$sampleRate " +
                   "channels=$channels bits=$bits seconds=${"%.3f".format(seconds)} " +
                   "peak=${"%.3f".format(peak / 32767.0)}")

        checkAsyncExport(host, folder, cd, rate)
    }

    /**
     * The same render, through export.start / export.status instead of the
     * blocking export.audio.
     *
     * The point of the split is that the engine thread is the thread the UI
     * talks to: waiting for a render inside a request froze the app for its
     * whole duration.  What can be checked from here is the state machine the
     * frontend polls - it has to start, report progress, finish, and hand back
     * the file - plus how long the engine took to answer while rendering.
     */
    private fun checkAsyncExport(host: EngineHost, folder: File, preset: String, rate: Long) {
        val params = JSONObject().put("folder", folder.absolutePath)
            .put("start", 0).put("end", 2 * rate).put("name", "async").put("preset", preset)
        val started = call(host, "export.start", params)
        if (!started.getBoolean("ok")) fail("the export did not start: $started")

        var polls = 0
        var maxRoundTrip = 0L
        var reply = started
        while (true) {
            val t0 = System.nanoTime()
            val s = call(host, "export.status")
            val dt = (System.nanoTime() - t0) / 1000000
            if (dt > maxRoundTrip) maxRoundTrip = dt
            reply = s
            if (s.getBoolean("finished")) break
            /* the engine must still answer while it renders: this call goes
             * through the same queue the UI uses */
            if (++polls > 600) fail("the export did not finish: $s")
            Thread.sleep(50)
        }
        if (!reply.getBoolean("succeeded")) fail("the async export failed: $reply")
        if (reply.getJSONArray("files").length() == 0) fail("the async export wrote no file: $reply")
        if (maxRoundTrip > 5000) fail("the engine blocked for ${maxRoundTrip}ms during the export")

        /* a finished job must not block the next one, and a long render has to
         * be cancellable while it runs */
        val long = JSONObject().put("folder", folder.absolutePath)
            .put("start", 0).put("end", 120 * rate).put("name", "cancel").put("preset", preset)
        val again = call(host, "export.start", long)
        if (!again.getBoolean("ok")) fail("the export cannot be started again: $again")
        val stop = call(host, "export.cancel")
        if (!stop.getBoolean("ok")) fail("the export could not be cancelled: $stop")
        var waited = 0
        while (!call(host, "export.status").getBoolean("finished")) {
            if (++waited > 400) fail("the cancelled export never finished")
            Thread.sleep(50)
        }
        val cancelled = call(host, "export.status")
        if (!cancelled.getBoolean("cancelled")) fail("the export was not cancelled: $cancelled")

        Log.i(TAG, "ARDOUR_EXPORT_ASYNC polls=$polls max_roundtrip=${maxRoundTrip}ms cancelled=true")
    }

    /**
     * The musical timeline: tempo, meter, the metronome, markers and the loop.
     *
     * The bar grid is the part worth checking: it is what the ruler draws, and
     * it has to agree with the tempo map rather than with the seconds.  Three
     * quarter notes at 96 bpm are 1.875 s per bar whatever the sample rate is,
     * and the engine is the only thing that knows it.
     */
    private fun checkMusical(host: EngineHost, rate: Long) {
        val start = call(host, "tempo.get")
        if (start.getDouble("bpm") <= 0) fail("the session has no tempo")
        if (start.getInt("beats_per_bar") <= 0) fail("the session has no meter")

        var m = call(host, "tempo.set", JSONObject().put("bpm", 96))
        if (!near(m.getDouble("bpm"), 96.0, 0.01)) fail("the tempo did not change: $m")
        m = call(host, "meter.set", JSONObject().put("beats", 3).put("unit", 4))
        if (m.getInt("beats_per_bar") != 3 || m.getInt("beat_unit") != 4) fail("the meter did not change: $m")

        val range = JSONObject().put("from", 0).put("to", 8 * rate)
        val bars = call(host, "grid", range).getJSONArray("bars")
        if (bars.length() < 4) fail("the bar grid is empty: $bars")
        val barLength = bars.getJSONObject(1).getLong("at") - bars.getJSONObject(0).getLong("at")
        val expected = 3.0 * 60.0 / 96.0 * rate
        if (!near(barLength.toDouble(), expected, rate * 0.002)) {
            fail("a bar is $barLength samples, expected $expected")
        }
        val beats = call(host, "grid", range).getJSONArray("beats")
        if (beats.length() < 2) fail("the beat grid is empty: $beats")

        if (!call(host, "click.set", JSONObject().put("enabled", true)).getBoolean("click")) {
            fail("the metronome did not start")
        }
        call(host, "click.set", JSONObject().put("enabled", false))

        val marker = call(host, "marker.add", JSONObject().put("position", 2 * rate).put("name", "Chorus"))
            .getJSONObject("marker")
        if (call(host, "markers.list").getJSONArray("markers").length() != 1) fail("the marker was not added")
        call(host, "marker.move", JSONObject().put("id", marker.getString("id")).put("position", 3 * rate))
        val marks = call(host, "markers.list").getJSONArray("markers")
        if (marks.getJSONObject(0).getLong("position") != 3 * rate) fail("the marker did not move: $marks")

        val loop = call(host, "loop.set", JSONObject().put("start", rate).put("end", 3 * rate).put("enabled", true))
            .getJSONObject("loop")
        if (loop.getLong("start") != rate || loop.getLong("end") != 3 * rate) fail("the loop has the wrong range: $loop")

        /* turning the loop on is a transport request, so it is answered by the
         * process thread a cycle or two later; and because Ardour treats the
         * loop as a mode, saying yes also starts the transport */
        var looped = false
        for (i in 0 until 30) {
            val t = call(host, "transport.state")
            if (t.optBoolean("loop")) {
                looped = true
                break
            }
            Thread.sleep(100)
        }
        if (!looped) fail("the transport did not start looping")

        if (!call(host, "loop.clear").isNull("loop")) fail("the loop was not cleared")
        call(host, "transport.stop")
        Thread.sleep(200)

        call(host, "marker.remove", JSONObject().put("id", marker.getString("id")))
        if (call(host, "markers.list").getJSONArray("markers").length() != 0) fail("the marker was not removed")

        Log.i(TAG, "ARDOUR_MUSICAL bpm=96 meter=3/4 bars=${bars.length()} barlen=$barLength " +
            "beats=${beats.length()} marks=1 loop=1")

        /* back to what the session started with, so nothing below sees this */
        call(host, "tempo.set", JSONObject().put("bpm", 120))
        call(host, "meter.set", JSONObject().put("beats", 4).put("unit", 4))
    }

    /** a request that has to be refused, for the error paths */
    private fun refused(host: EngineHost, method: String, params: JSONObject) {
        val reply = JSONObject(host.call(method, params.toString()))
        if (reply.optBoolean("ok", false)) fail("$method was accepted but should not have been: $reply")
        Log.i(TAG, "$method refused: ${reply.optString("error")}")
    }

    /**
     * Sends: one track feeding a bus.  A send is a processor in the source's
     * chain, so pre and post mean on one side of the fader or the other;
     * flipping it has to move the processor and keep the level.
     */
    private fun checkSends(host: EngineHost, track: String, bus: String) {
        fun sendsOf(id: String): org.json.JSONArray {
            val strips = call(host, "mixer.strips").getJSONArray("strips")
            for (i in 0 until strips.length()) {
                if (strips.getJSONObject(i).getString("id") == id) return strips.getJSONObject(i).getJSONArray("sends")
            }
            fail("strip $id is not in the mixer")
        }

        if (sendsOf(track).length() != 0) fail("a new track already has sends")
        refused(host, "send.add", JSONObject().put("track", track).put("target", track))

        var strip = call(host, "send.add", JSONObject().put("track", track).put("target", bus)).getJSONObject("strip")
        var sends = strip.getJSONArray("sends")
        if (sends.length() != 1) fail("the send was not created: $strip")
        if (sends.getJSONObject(0).getString("target") != bus) fail("the send points somewhere else: $sends")
        if (sends.getJSONObject(0).getBoolean("pre")) fail("the send should be post-fader by default: $sends")

        refused(host, "send.add", JSONObject().put("track", track).put("target", bus))

        strip = call(host, "send.set",
            JSONObject().put("track", track).put("target", bus).put("gain_db", -12)).getJSONObject("strip")
        sends = strip.getJSONArray("sends")
        if (!near(sends.getJSONObject(0).getDouble("gain_db"), -12.0, 0.1)) fail("the send level did not apply: $sends")

        strip = call(host, "send.set",
            JSONObject().put("track", track).put("target", bus).put("pre", true)).getJSONObject("strip")
        sends = strip.getJSONArray("sends")
        if (!sends.getJSONObject(0).getBoolean("pre")) fail("the send did not move in front of the fader: $sends")
        if (!near(sends.getJSONObject(0).getDouble("gain_db"), -12.0, 0.1)) fail("moving the send lost its level: $sends")

        strip = call(host, "send.remove", JSONObject().put("track", track).put("target", bus)).getJSONObject("strip")
        if (strip.getJSONArray("sends").length() != 0) fail("the send was not removed: $strip")

        Log.i(TAG, "ARDOUR_SENDS add/set/move/remove ok on $track -> $bus")
    }

    private fun checkEditing(host: EngineHost, scratch: File) {
        val rate = call(host, "engine.status").optLong("sample_rate", 48000L)

        call(host, "session.create", JSONObject().put("name", "ui-" + System.currentTimeMillis()))
        val vocals = call(host, "tracks.add", JSONObject().put("name", "Vocals").put("channels", 1)).getString("id")
        val guitar = call(host, "tracks.add", JSONObject().put("name", "Guitar").put("channels", 2)).getString("id")
        val temp = call(host, "tracks.add", JSONObject().put("name", "Temp").put("channels", 1)).getString("id")
        val bus = call(host, "buses.add", JSONObject().put("name", "Reverb")).getJSONObject("strip").getString("id")
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
        checkSends(host, vocals, bus)

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
        checkMidiAndPlugins(host, rate, guitar)
        checkAutomation(host, rate, guitar)
        checkMusical(host, rate)
        checkExport(host, scratch, rate)

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
        /* keep rolling for a while after the meter reads: the service watches
         * the transport every two seconds to hold its wake lock, and CI reads
         * that decision out of the log, so the window has to be wider than the
         * watch interval rather than "as short as possible" */
        Thread.sleep(5000)
        call(host, "transport.stop")
        if (heard <= -30) fail("the tone did not reach the track meter (peak $heard dBFS)")
        Log.i(TAG, "track meter peak $heard dBFS while the tone played")
        logAudioPath(host, "after-tone")
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

    /**
     * The block size.  The engine has to report the sizes its backend offers,
     * take a new one by stopping and starting itself, and come back with the
     * old one when asked.  A backend that cannot resize is not a failure - the
     * dummy fallback on an emulator has nothing to resize - but the engine has
     * to survive the attempt either way.
     */
    private fun checkBufferSize(host: EngineHost) {
        val before = call(host, "engine.buffer")
        val sizes = before.getJSONArray("sizes")
        val current = before.getInt("samples")
        val backend = call(host, "engine.status").optString("backend")

        var other = 0
        for (i in 0 until sizes.length()) {
            val n = sizes.getInt(i)
            if (n != current) {
                other = n
                break
            }
        }
        if (sizes.length() == 0 || other == 0) {
            Log.i(TAG, "ARDOUR_BUFFER skipped backend=\"$backend\" sizes=$sizes")
            return
        }

        val changed = JSONObject(host.call("engine.buffer", JSONObject().put("samples", other).toString()))
        if (!changed.optBoolean("ok", false)) {
            /* a backend that cannot resize says so; what matters is that the
             * engine is still there afterwards */
            if (!call(host, "engine.status").optBoolean("running")) {
                fail("the engine stopped after a refused block size: ${changed.optString("error")}")
            }
            Log.i(TAG, "ARDOUR_BUFFER skipped backend=\"$backend\" reason=${changed.optString("error")}")
            return
        }
        if (changed.optInt("buffer_size", -1) != other) {
            fail("the block size did not become $other: $changed")
        }

        val back = call(host, "engine.buffer", JSONObject().put("samples", current))
        if (back.optInt("buffer_size", -1) != current) {
            fail("the block size did not go back to $current: $back")
        }
        Log.i(TAG, "ARDOUR_BUFFER samples=$current -> $other -> $current backend=\"$backend\"")
    }

    private fun check(host: EngineHost) {
        host.start().get()

        val status = call(host, "engine.status")
        if (!status.optBoolean("running")) throw IllegalStateException("the engine is not running")
        Log.i(TAG, "backend ${status.optString("backend")} at ${status.optInt("sample_rate")} Hz")

        checkBufferSize(host)

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
        logAudioPath(host, "rolling")
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
