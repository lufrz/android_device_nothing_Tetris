/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris.network

internal enum class TcpFailure {
    UNAVAILABLE, INVALID_ALGORITHM, CONFLICT, WRITE_FAILED, READBACK_FAILED, NOT_OWNED,
    PERSIST_FAILED,
}

internal data class TcpState(
    val current: String?,
    val available: List<String>,
    val initial: String?,
    val canApply: Boolean,
    val canRestore: Boolean,
    val unavailableReason: TcpFailure?,
    val bootAlgorithm: String? = null,
)

internal data class TcpResult(
    val state: TcpState,
    val failure: TcpFailure? = null,
    val writeAttempted: Boolean = false,
) {
    val successful: Boolean get() = failure == null
}

/** The property request is deliberately separate from the kernel's actual value. */
internal interface TcpControlPort {
    fun readCurrent(): String
    fun readAvailable(): String
    fun controlReady(): Boolean
    fun clearPendingRequests(): Boolean
    fun request(algorithm: String)
    fun restoreAfterConsent(algorithm: String) = request(algorithm)
    fun readBootAlgorithm(): String
    fun saveBootAlgorithm(algorithm: String)
}

/**
 * Changes the default for new TCP sockets. Boot restoration is opt-in and handled by init.
 * Call on a worker thread: init applies requests asynchronously, so success requires readback.
 */
internal class TcpControlManager(
    private val port: TcpControlPort,
    private val pause: (Long) -> Unit = { Thread.sleep(it) },
) {
    private var initial: String? = null
    private var owned: String? = null

    @Synchronized fun readState(): TcpState = snapshot()

    /** Disable persistence and undo only this process's still-owned live value. */
    @Synchronized fun disableForConsent(): Boolean {
        val cleared = runCatching { port.clearPendingRequests() }.getOrDefault(false)
        val persisted = runCatching {
            port.saveBootAlgorithm("off")
            port.readBootAlgorithm() == "off"
        }.getOrDefault(false)
        val before = snapshot()
        val target = initial
        if (owned == null || owned != before.current || target == null || target == before.current)
            return persisted && cleared
        if (!cleared || target !in before.available) return false
        val restored = runCatching {
            // Unlike ordinary requests, restoration remains available after the gate closes.
            if (port.readCurrent().trim() != owned) return@runCatching true
            port.restoreAfterConsent(target)
            repeat(26) { attempt ->
                val current = port.readCurrent().trim()
                if (current == target) { owned = null; return@runCatching true }
                if (current != before.current) { owned = null; return@runCatching false }
                if (attempt < 25) pause(40)
            }
            false
        }.getOrDefault(false)
        // A restore command is also one-shot: a later revocation must never replay
        // this old baseline over a value selected by another controller.
        val neutral = runCatching { port.clearPendingRequests() }.getOrDefault(false)
        return persisted && restored && neutral
    }

    @Synchronized fun apply(target: String, expected: TcpState): TcpResult {
        val before = snapshot()
        if (target !in SUPPORTED || target !in before.available) {
            return TcpResult(before, TcpFailure.INVALID_ALGORITHM)
        }
        return rememberSelection(applyChecked(target, expected, before))
    }

    @Synchronized fun restoreInitial(expected: TcpState): TcpResult {
        val before = snapshot()
        if (before.current != expected.current || before.available != expected.available) {
            return TcpResult(before, TcpFailure.CONFLICT)
        }
        if (!before.canRestore) return TcpResult(before, TcpFailure.NOT_OWNED)
        return rememberSelection(applyChecked(checkNotNull(initial), expected, before))
    }

    @Synchronized fun setApplyOnBoot(enabled: Boolean, expected: TcpState): TcpResult {
        val before = snapshot()
        // Disabling persistence must remain possible even if kernel control is unavailable.
        if (!enabled) return saveBootSelection("off", TcpResult(before))
        if (!before.canApply) return TcpResult(before, TcpFailure.UNAVAILABLE)
        if (before.current != expected.current || before.available != expected.available) {
            return TcpResult(before, TcpFailure.CONFLICT)
        }
        val target = before.current
        if (target == null || target !in before.available) {
            return TcpResult(before, TcpFailure.INVALID_ALGORITHM)
        }
        return saveBootSelection(target, TcpResult(before))
    }

    private fun rememberSelection(result: TcpResult): TcpResult {
        if (!result.successful || result.state.bootAlgorithm == null) return result
        val target = checkNotNull(result.state.current)
        return if (target == result.state.bootAlgorithm) result else saveBootSelection(target, result)
    }

    private fun saveBootSelection(target: String, result: TcpResult): TcpResult {
        val saved = runCatching {
            port.saveBootAlgorithm(target)
            port.readBootAlgorithm() == target
        }.getOrDefault(false)
        return result.copy(state = snapshot(), failure = if (saved) null else TcpFailure.PERSIST_FAILED)
    }

    private fun applyChecked(target: String, expected: TcpState, before: TcpState): TcpResult {
        if (!before.canApply) return TcpResult(before, TcpFailure.UNAVAILABLE)
        if (before.current != expected.current || before.available != expected.available) {
            return TcpResult(before, TcpFailure.CONFLICT)
        }
        if (before.current == target) return TcpResult(before)

        // Sysctl has no atomic compare-and-swap; recheck immediately before requesting a write.
        val guarded = snapshot()
        if (!guarded.canApply || guarded.current != before.current ||
            guarded.available != before.available) {
            return TcpResult(guarded, TcpFailure.CONFLICT)
        }
        try {
            port.request(target)
        } catch (_: Exception) {
            return TcpResult(snapshot(), TcpFailure.WRITE_FAILED, writeAttempted = true)
        }

        var previousMatched = false
        repeat(26) { attempt ->
            val observed = snapshot()
            if (!observed.canApply) {
                return TcpResult(observed, TcpFailure.READBACK_FAILED, writeAttempted = true)
            }
            if (observed.available != before.available ||
                observed.current !in listOf(before.current, target)) {
                owned = null
                return TcpResult(observed.copy(canRestore = false), TcpFailure.CONFLICT, true)
            }
            if (observed.current == target) {
                if (previousMatched) {
                    owned = target.takeUnless { it == initial }
                    return TcpResult(observed.copy(canRestore = canRestore(observed)),
                        writeAttempted = true)
                }
                previousMatched = true
            } else {
                // A target observed and then replaced belongs to another controller.
                if (previousMatched) {
                    owned = null
                    return TcpResult(observed.copy(canRestore = false), TcpFailure.CONFLICT, true)
                }
            }
            if (attempt < 25) {
                try {
                    pause(40)
                } catch (_: InterruptedException) {
                    Thread.currentThread().interrupt()
                    return TcpResult(snapshot(), TcpFailure.READBACK_FAILED, true)
                }
            }
        }
        // Never roll back a timeout: a late init action or an external writer may still own it.
        return TcpResult(snapshot(), TcpFailure.READBACK_FAILED, writeAttempted = true)
    }

    private fun snapshot(): TcpState {
        // Observe readiness before reading the value restored by init during boot.
        val ready = runCatching { port.controlReady() }.getOrDefault(false)
        val current = runCatching { port.readCurrent().trim() }
            .getOrNull()?.takeIf { NAME.matches(it) }
        val rawAvailable = runCatching { port.readAvailable().trim() }.getOrNull()
        val available = rawAvailable?.takeIf { it.length <= 1024 }
            ?.split(Regex("\\s+"))?.takeIf { values ->
                values.isNotEmpty() && values.all { NAME.matches(it) }
            }?.toSet()?.let { values -> SUPPORTED.filter { it in values } }.orEmpty()
        val bootAlgorithm = runCatching { port.readBootAlgorithm() }.getOrNull()
            ?.takeIf { it in SUPPORTED }
        if (initial == null && ready && current != null) initial = current
        if (owned != null && current != owned) owned = null
        val applicable = current != null && available.isNotEmpty() && ready
        val state = TcpState(current, available, initial, applicable, false,
            if (applicable) null else TcpFailure.UNAVAILABLE, bootAlgorithm)
        return state.copy(canRestore = canRestore(state))
    }

    private fun canRestore(state: TcpState): Boolean = state.canApply && owned != null &&
        owned == state.current && initial in state.available && initial != state.current

    companion object {
        // Must match the fixed-value init actions. Unknown algorithms remain read-only.
        val SUPPORTED = listOf("reno", "cubic", "bic", "westwood", "htcp", "bbr", "bbr3")
        private val NAME = Regex("[a-zA-Z0-9_-]{1,15}")
    }
}
