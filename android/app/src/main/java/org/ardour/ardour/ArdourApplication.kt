package org.ardour.ardour

import android.app.Application

/** Holds the engine for the lifetime of the process, across activity restarts. */
class ArdourApplication : Application() {
    val engine: EngineHost by lazy { EngineHost(this) }
    /** external MIDI devices, opened once for the whole process */
    val midi: MidiInput by lazy { MidiInput(this, engine).also { it.start() } }
}
