/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.battery

import org.lineageos.settings.tetris.consent.PartsControlGate

internal data class ChargeBypassStatus(
    val requested: Boolean,
    val online: Boolean,
    val active: Boolean,
    val powerPath: Boolean,
    val charging: Boolean,
    val error: Int,
) {
    companion object {
        /** Unknown versions, duplicate fields and inconsistent active claims are unavailable. */
        fun parse(raw: String): ChargeBypassStatus? {
            if (raw.length > 512) return null
            val line = raw.trim()
            if (line.isEmpty() || '\n' in line || '\r' in line) return null
            val values = mutableMapOf<String, String>()
            for (entry in line.split(Regex("[ \\t]+"))) {
                val pair = entry.split('=')
                if (pair.size != 2 || values.put(pair[0], pair[1]) != null) return null
            }
            if (values.keys != setOf("version", "requested", "online", "active", "power_path", "charging", "error") ||
                values["version"] != "1") return null
            fun bit(key: String): Boolean? = when (values[key]) { "0" -> false; "1" -> true; else -> null }
            val requested = bit("requested") ?: return null
            val online = bit("online") ?: return null
            val active = bit("active") ?: return null
            val powerPath = bit("power_path") ?: return null
            val charging = bit("charging") ?: return null
            val error = values["error"]?.takeIf { it.matches(Regex("-?[0-9]+")) }
                ?.toIntOrNull()?.takeIf { it in -4095..4095 } ?: return null
            if (active && (!requested || !online || !powerPath || charging || error != 0)) return null
            return ChargeBypassStatus(requested, online, active, powerPath, charging, error)
        }
    }
}

internal interface ChargeBypassPort {
    fun exists(): Boolean
    fun writable(): Boolean
    fun readRequested(): String
    fun readStatus(): String
    fun writeRequested(requested: Boolean)
}

internal data class ChargeBypassState(
    val status: ChargeBypassStatus? = null,
    val requested: Boolean? = null,
    val writable: Boolean = false,
    val releasePending: Boolean = false,
) {
    val canStop: Boolean get() = requested == true || releasePending
    val canEnable: Boolean get() = status?.online == true && requested == false && writable
}

internal enum class ChargeBypassFailure { CONSENT_REQUIRED, UNAVAILABLE, WRITE_FAILED, READBACK_FAILED }
internal data class ChargeBypassResult(val state: ChargeBypassState, val failure: ChargeBypassFailure? = null) {
    val successful: Boolean get() = failure == null
}

/** The only writable value is the dedicated Parts inhibit; ordinary charging controls are untouched. */
internal class ChargeBypassManager(private val port: ChargeBypassPort) {
    // Retain a recovery action if a write may have succeeded but its readback was lost.
    private var releasePending = false

    @Synchronized fun readState(): ChargeBypassState {
        val status = runCatching { ChargeBypassStatus.parse(port.readStatus()) }.getOrNull()
        val request = runCatching { parseRequested(port.readRequested()) }.getOrNull()
        if (request == false) releasePending = false
        if (request == true || (request == null && status?.requested == true)) releasePending = true
        // Two independent reads can straddle a kernel update. Do not claim active from a mixed sample.
        val consistentStatus = status?.takeIf { request != null && it.requested == request }
        return ChargeBypassState(consistentStatus, request ?: status?.requested,
            runCatching { port.writable() }.getOrDefault(false), releasePending)
    }

    @Synchronized fun setRequested(requested: Boolean): ChargeBypassResult {
        if (!requested) return release()
        // Hold the consent mutation gate through readback; reset waits for this transaction.
        return PartsControlGate.runIfAllowed {
            val before = readState()
            if (before.requested == true && before.status != null) return@runIfAllowed ChargeBypassResult(before)
            if (!before.canEnable) return@runIfAllowed ChargeBypassResult(before, ChargeBypassFailure.UNAVAILABLE)
            releasePending = true
            try {
                port.writeRequested(true)
            } catch (_: Exception) {
                return@runIfAllowed ChargeBypassResult(readState(), ChargeBypassFailure.WRITE_FAILED)
            }
            val after = readState()
            // Acknowledged request is distinct from activation. Errors/paused status stay visible.
            ChargeBypassResult(after, if (after.requested == true && after.status?.requested == true) null
                else ChargeBypassFailure.READBACK_FAILED)
        } ?: ChargeBypassResult(readState(), ChargeBypassFailure.CONSENT_REQUIRED)
    }

    /** Valid without consent, online status or readable telemetry; releases only this API's vote. */
    @Synchronized fun disableForConsent(): Boolean = release().successful

    private fun release(): ChargeBypassResult {
        val before = readState()
        if (before.requested == false && !before.releasePending) return ChargeBypassResult(before)
        if (!before.canStop && !runCatching { port.exists() }.getOrDefault(false)) return ChargeBypassResult(before)
        try {
            port.writeRequested(false)
        } catch (_: Exception) {
            return ChargeBypassResult(readState(), ChargeBypassFailure.WRITE_FAILED)
        }
        val after = readState()
        return ChargeBypassResult(after, if (after.requested == false && !after.releasePending) null
            else ChargeBypassFailure.READBACK_FAILED)
    }

    private fun parseRequested(raw: String): Boolean? = when (raw.trim()) {
        "0" -> false
        "1" -> true
        else -> null
    }
}
