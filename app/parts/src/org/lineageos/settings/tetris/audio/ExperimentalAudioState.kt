// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.audio

enum class ExperimentalAudioMode(val wireValue: String) {
    OFF("off"), DUAL_MONO("dual_mono"), STEREO("stereo");

    companion object {
        fun fromWire(value: String?) = entries.firstOrNull { it.wireValue == value }
    }
}

enum class ExperimentalAudioOutput(val wireValue: String) {
    BOTH("both"), UPPER("upper"), LOWER("lower");

    companion object {
        fun fromWire(value: String?) = entries.firstOrNull { it.wireValue == value }
    }
}

internal data class ExperimentalAudioState(
    val available: Boolean = false,
    val stereoAvailable: Boolean = false,
    val requested: ExperimentalAudioMode = ExperimentalAudioMode.OFF,
    val active: ExperimentalAudioMode = ExperimentalAudioMode.OFF,
    val reason: String = "unavailable",
    val gainAvailable: Boolean = false,
    val gainDb: Int = 0,
    val softwareGainAvailable: Boolean = false,
    val softwareGainDb: Int = 0,
    val syncTrimAvailable: Boolean = false,
    val syncTrimMs: Int = 0,
    val outputAvailable: Boolean = false,
    val output: ExperimentalAudioOutput = ExperimentalAudioOutput.BOTH,
    val outputActive: ExperimentalAudioOutput = ExperimentalAudioOutput.BOTH,
) {
    val usesSeparateOutput: Boolean
        get() = requested != ExperimentalAudioMode.OFF &&
            (requested == ExperimentalAudioMode.STEREO || output != ExperimentalAudioOutput.BOTH)

    companion object {
        const val MIN_GAIN_DB = -10
        const val MAX_GAIN_DB = 6
        const val MIN_SOFTWARE_GAIN_DB = 0
        const val MAX_SOFTWARE_GAIN_DB = 6
        const val MIN_SYNC_TRIM_MS = -10
        const val MAX_SYNC_TRIM_MS = 10

        fun parse(parameters: String?): ExperimentalAudioState? {
            if (parameters.isNullOrBlank() || parameters.length > 4096) return null
            val fields = mutableMapOf<String, String>()
            for (entry in parameters.split(';').filter { it.isNotBlank() }) {
                val pair = entry.split('=', limit = 2)
                if (pair.size != 2) return null
                val key = pair[0].trim()
                if (!key.startsWith("tetris_audio_")) continue
                if (fields.put(key.removePrefix("tetris_audio_"), pair[1].trim()) != null) return null
            }
            if (fields["version"] != "1") return null
            fun flag(name: String): Boolean? = when (fields[name]) {
                "0" -> false
                "1" -> true
                else -> null
            }
            val available = flag("available") ?: return null
            val stereoAvailable = flag("stereo_available") ?: return null
            val requested = ExperimentalAudioMode.fromWire(fields["requested"]) ?: return null
            val active = ExperimentalAudioMode.fromWire(fields["active"]) ?: return null
            val reason = fields["reason"]?.takeIf { it.matches(Regex("[a-z_]{1,64}")) }
                ?: return null
            if (stereoAvailable && !available) return null
            if (active != ExperimentalAudioMode.OFF && (!available || active != requested)) return null
            if (active == ExperimentalAudioMode.STEREO && !stereoAvailable) return null
            val hasGain = "gain_available" in fields || "gain_db" in fields
            val gainAvailable = if (hasGain) flag("gain_available") ?: return null else false
            val gainDb = if (hasGain) fields["gain_db"]?.toIntOrNull() ?: return null else 0
            if (gainDb !in MIN_GAIN_DB..MAX_GAIN_DB || (gainAvailable && !available)) return null
            val hasSoftwareGain = "software_gain_available" in fields || "software_gain_db" in fields
            val softwareGainAvailable = if (hasSoftwareGain) flag("software_gain_available") ?: return null else false
            val softwareGainDb = if (hasSoftwareGain) fields["software_gain_db"]?.toIntOrNull() ?: return null else 0
            if (softwareGainDb !in MIN_SOFTWARE_GAIN_DB..MAX_SOFTWARE_GAIN_DB ||
                (softwareGainAvailable && !stereoAvailable)) return null
            val hasSyncTrim = "sync_trim_available" in fields || "sync_trim_ms" in fields
            val syncTrimAvailable = if (hasSyncTrim) flag("sync_trim_available") ?: return null else false
            val syncTrimMs = if (hasSyncTrim) fields["sync_trim_ms"]?.toIntOrNull() ?: return null else 0
            if (syncTrimMs !in MIN_SYNC_TRIM_MS..MAX_SYNC_TRIM_MS ||
                (syncTrimAvailable && !stereoAvailable)) return null
            val hasOutput = "output_available" in fields || "output" in fields
            val outputAvailable = if (hasOutput) flag("output_available") ?: return null else false
            val output = if (hasOutput) ExperimentalAudioOutput.fromWire(fields["output"]) ?: return null
                else ExperimentalAudioOutput.BOTH
            if (outputAvailable && !stereoAvailable) return null
            val outputActive = if ("output_active" in fields)
                ExperimentalAudioOutput.fromWire(fields["output_active"]) ?: return null else output
            return ExperimentalAudioState(available, stereoAvailable, requested, active, reason,
                gainAvailable, gainDb, softwareGainAvailable, softwareGainDb,
                syncTrimAvailable, syncTrimMs, outputAvailable, output, outputActive)
        }
    }
}

internal interface ExperimentalAudioTransport {
    fun read(): String?
    fun write(mode: ExperimentalAudioMode): Int
    fun writeGain(gainDb: Int): Int = -1
    fun writeSoftwareGain(gainDb: Int): Int = -1
    fun writeSyncTrim(trimMs: Int): Int = -1
    fun writeOutput(output: ExperimentalAudioOutput): Int = -1
}

internal data class ExperimentalAudioResult(val state: ExperimentalAudioState, val accepted: Boolean)

/** The HAL owns the route lifetime. Reads never replay a saved choice or change routing. */
internal class ExperimentalAudioControl(private val transport: ExperimentalAudioTransport) {
    private var last = ExperimentalAudioState()

    @Synchronized
    fun read(): ExperimentalAudioState {
        val next = runCatching { ExperimentalAudioState.parse(transport.read()) }.getOrNull()
        // Keep a previously enabled toggle switchable OFF when the service is temporarily
        // unreachable. Do not claim the old active route is still present.
        last = next ?: last.copy(available = false, stereoAvailable = false, gainAvailable = false,
            softwareGainAvailable = false, syncTrimAvailable = false, outputAvailable = false,
            active = ExperimentalAudioMode.OFF, reason = "unavailable")
        return last
    }

    @Synchronized
    fun apply(mode: ExperimentalAudioMode): ExperimentalAudioResult {
        val before = read()
        if (mode != ExperimentalAudioMode.OFF && (!before.available ||
                (mode == ExperimentalAudioMode.STEREO && !before.stereoAvailable))) {
            return ExperimentalAudioResult(before, false)
        }
        val success = runCatching { transport.write(mode) == 0 }.getOrDefault(false)
        val after = read()
        // Other audio HALs may accept unknown parameter names. Only the versioned
        // response and matching readback acknowledge a change.
        return ExperimentalAudioResult(after, success && after.reason != "unavailable" &&
            after.requested == mode)
    }

    @Synchronized
    fun applyGain(gainDb: Int): ExperimentalAudioResult {
        val before = read()
        if (!before.available || !before.gainAvailable ||
                gainDb !in ExperimentalAudioState.MIN_GAIN_DB..ExperimentalAudioState.MAX_GAIN_DB) {
            return ExperimentalAudioResult(before, false)
        }
        val success = runCatching { transport.writeGain(gainDb) == 0 }.getOrDefault(false)
        val after = read()
        return ExperimentalAudioResult(after, success && after.gainAvailable && after.gainDb == gainDb)
    }

    @Synchronized
    fun applySoftwareGain(gainDb: Int): ExperimentalAudioResult {
        val before = read()
        if (!before.softwareGainAvailable || !before.usesSeparateOutput || before.output == ExperimentalAudioOutput.LOWER ||
                gainDb !in ExperimentalAudioState.MIN_SOFTWARE_GAIN_DB..ExperimentalAudioState.MAX_SOFTWARE_GAIN_DB) {
            return ExperimentalAudioResult(before, false)
        }
        val success = runCatching { transport.writeSoftwareGain(gainDb) == 0 }.getOrDefault(false)
        val after = read()
        return ExperimentalAudioResult(after, success && after.softwareGainAvailable && after.softwareGainDb == gainDb)
    }

    @Synchronized
    fun applySyncTrim(trimMs: Int): ExperimentalAudioResult {
        val before = read()
        if (!before.syncTrimAvailable || before.requested != ExperimentalAudioMode.STEREO ||
                before.output != ExperimentalAudioOutput.BOTH ||
                trimMs !in ExperimentalAudioState.MIN_SYNC_TRIM_MS..ExperimentalAudioState.MAX_SYNC_TRIM_MS) {
            return ExperimentalAudioResult(before, false)
        }
        val success = runCatching { transport.writeSyncTrim(trimMs) == 0 }.getOrDefault(false)
        val after = read()
        return ExperimentalAudioResult(after, success && after.syncTrimAvailable && after.syncTrimMs == trimMs)
    }

    @Synchronized
    fun applyOutput(output: ExperimentalAudioOutput): ExperimentalAudioResult {
        val before = read()
        if (!before.outputAvailable || before.requested == ExperimentalAudioMode.OFF) {
            return ExperimentalAudioResult(before, false)
        }
        val success = runCatching { transport.writeOutput(output) == 0 }.getOrDefault(false)
        val after = read()
        return ExperimentalAudioResult(after, success && after.outputAvailable && after.output == output)
    }
}
