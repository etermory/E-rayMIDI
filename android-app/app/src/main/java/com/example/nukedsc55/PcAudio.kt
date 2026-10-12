package com.example.nukedsc55

/** Shared by all five native engines; controlled on the Activity's main thread. */
object PcAudio {
    init { System.loadLibrary("pc-audio") }
    external fun nativeSetMode(mode: Int): Boolean
    external fun nativeStatus(): Int
}
