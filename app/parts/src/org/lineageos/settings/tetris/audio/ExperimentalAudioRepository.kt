// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.audio

import android.media.AudioSystem
import android.os.SystemProperties
import org.lineageos.settings.tetris.consent.PartsControlGate

internal object ExperimentalAudioRepository {
    private const val MODE_PROPERTY = "persist.sys.tetris.audio_mode"
    private const val GAIN_PROPERTY = "persist.sys.tetris.audio_gain"
    private const val SOFTWARE_GAIN_PROPERTY = "persist.sys.tetris.audio_software_gain"
    private const val SYNC_TRIM_PROPERTY = "persist.sys.tetris.audio_sync_trim"
    private const val OUTPUT_PROPERTY = "persist.sys.tetris.audio_output"

    private fun writeWithConsent(action: () -> Int): Int =
        PartsControlGate.runIfAllowed(action) ?: -1

    private fun writePreference(property: String, parameter: String, value: String, previous: String,
            matches: (ExperimentalAudioState) -> Boolean): Int {
        SystemProperties.set(property, value)
        var acknowledged = false
        try {
            val result = AudioSystem.setParameters("$parameter=$value")
            val observed = ExperimentalAudioState.parse(AudioSystem.getParameters("tetris_audio"))
            acknowledged = result == 0 && observed != null && matches(observed)
            return if (acknowledged) 0 else -1
        } finally {
            if (!acknowledged && SystemProperties.get(property) == value) {
                SystemProperties.set(property, previous)
                if (SystemProperties.get(property) == previous) {
                    runCatching { AudioSystem.setParameters("$parameter=$previous") }
                }
            }
        }
    }

    private fun savedNumber(property: String, range: IntRange): String =
        SystemProperties.get(property, "0").toIntOrNull()?.takeIf { it in range }?.toString() ?: "0"
    val control = ExperimentalAudioControl(object : ExperimentalAudioTransport {
        override fun read(): String? = AudioSystem.getParameters("tetris_audio")
        override fun write(mode: ExperimentalAudioMode): Int = writeWithConsent {
            // The vendor HAL cannot recover the original app UID from AudioFlinger.
            // This restricted property authorizes the request before forwarding it.
            val previous = SystemProperties.get(MODE_PROPERTY, "off")
            SystemProperties.set(MODE_PROPERTY, mode.wireValue)
            var acknowledged = false
            try {
                val result = AudioSystem.setParameters("tetris_audio_mode=${mode.wireValue}")
                val observed = ExperimentalAudioState.parse(read())
                acknowledged = result == 0 && observed?.requested == mode
                return@writeWithConsent if (acknowledged) 0 else -1
            } finally {
                if (!acknowledged && SystemProperties.get(MODE_PROPERTY) == mode.wireValue) {
                    SystemProperties.set(MODE_PROPERTY, previous)
                    // The HAL may have accepted the write before its reply was lost.
                    // Restore its live selection as well as the value used on restart.
                    // A newer authorization wins; the HAL checks it again on receipt.
                    if (SystemProperties.get(MODE_PROPERTY) == previous) {
                        runCatching {
                            AudioSystem.setParameters("tetris_audio_mode=$previous")
                        }
                    }
                }
            }
        }
        override fun writeGain(gainDb: Int): Int = writeWithConsent {
            if (gainDb !in ExperimentalAudioState.MIN_GAIN_DB..ExperimentalAudioState.MAX_GAIN_DB) return@writeWithConsent -1
            val value = gainDb.toString()
            // Match the HAL's restore behavior for preferences from older builds.
            val previous = SystemProperties.get(GAIN_PROPERTY, "0").let {
                if (it == "7" || it == "8") ExperimentalAudioState.MAX_GAIN_DB.toString() else it
            }
            SystemProperties.set(GAIN_PROPERTY, value)
            var acknowledged = false
            try {
                val result = AudioSystem.setParameters("tetris_audio_gain=$value")
                val observed = ExperimentalAudioState.parse(read())
                acknowledged = result == 0 && observed?.gainAvailable == true && observed.gainDb == gainDb
                return@writeWithConsent if (acknowledged) 0 else -1
            } finally {
                if (!acknowledged && SystemProperties.get(GAIN_PROPERTY) == value) {
                    SystemProperties.set(GAIN_PROPERTY, previous)
                    if (SystemProperties.get(GAIN_PROPERTY) == previous) {
                        runCatching { AudioSystem.setParameters("tetris_audio_gain=$previous") }
                    }
                }
            }
        }
        override fun writeSoftwareGain(gainDb: Int): Int = writeWithConsent {
            val range = ExperimentalAudioState.MIN_SOFTWARE_GAIN_DB..ExperimentalAudioState.MAX_SOFTWARE_GAIN_DB
            if (gainDb !in range) return@writeWithConsent -1
            return@writeWithConsent writePreference(SOFTWARE_GAIN_PROPERTY, "tetris_audio_software_gain", gainDb.toString(),
                savedNumber(SOFTWARE_GAIN_PROPERTY, range)) { it.softwareGainAvailable && it.softwareGainDb == gainDb }
        }
        override fun writeSyncTrim(trimMs: Int): Int = writeWithConsent {
            val range = ExperimentalAudioState.MIN_SYNC_TRIM_MS..ExperimentalAudioState.MAX_SYNC_TRIM_MS
            if (trimMs !in range) return@writeWithConsent -1
            return@writeWithConsent writePreference(SYNC_TRIM_PROPERTY, "tetris_audio_sync_trim", trimMs.toString(),
                savedNumber(SYNC_TRIM_PROPERTY, range)) { it.syncTrimAvailable && it.syncTrimMs == trimMs }
        }
        override fun writeOutput(output: ExperimentalAudioOutput): Int = writeWithConsent {
            val previous = ExperimentalAudioOutput.fromWire(SystemProperties.get(OUTPUT_PROPERTY, "both"))
                ?: ExperimentalAudioOutput.BOTH
            return@writeWithConsent writePreference(OUTPUT_PROPERTY, "tetris_audio_output", output.wireValue, previous.wireValue) {
                it.outputAvailable && it.output == output
            }
        }
    })
}
