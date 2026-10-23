package org.ardour.ardour

import android.app.Application

/** Holds the engine for the lifetime of the process, across activity restarts. */
class ArdourApplication : Application() {
    val engine: EngineHost by lazy { EngineHost(this) }
}
