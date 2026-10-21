/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris.network

import java.io.IOException
import java.util.concurrent.CountDownLatch
import kotlin.concurrent.thread

private class Port : TcpControlPort {
    var current = "cubic"
    var available = "reno cubic westwood bbr bbr3 custom"
    var ready = true
    var readable = true
    var bootAlgorithm = "off"
    var liveRequest = "off"
    var restoreRequest = "off"
    var clearSucceeds = true
    val writes = mutableListOf<String>()
    val bootSaves = mutableListOf<String>()
    var action: (String) -> Unit = { current = it }
    var saveAction: (String) -> Unit = { bootAlgorithm = it }
    override fun readCurrent(): String = if (readable) current else throw IOException("missing")
    override fun readAvailable() = available
    override fun controlReady() = ready
    override fun clearPendingRequests(): Boolean {
        if (!clearSucceeds) return false
        liveRequest = "off"; restoreRequest = "off"
        return true
    }
    fun consentChanged(accepted: Boolean) {
        val replay = if (accepted) liveRequest else restoreRequest
        if (replay != "off") { writes += replay; action(replay) }
    }
    override fun restoreAfterConsent(algorithm: String) {
        restoreRequest = algorithm
        writes += algorithm
        action(algorithm)
    }
    override fun request(algorithm: String) {
        liveRequest = algorithm
        writes += algorithm
        action(algorithm)
    }
    override fun readBootAlgorithm() = bootAlgorithm
    override fun saveBootAlgorithm(algorithm: String) {
        bootSaves += algorithm
        saveAction(algorithm)
    }
}

private fun checkFailure(result: TcpResult, failure: TcpFailure) {
    check(result.failure == failure) { "Expected $failure, got $result" }
    check(!result.successful)
}

fun main() {
    var cases = 0
    fun test(name: String, block: () -> Unit) {
        block()
        cases++
        println("PASS: $name")
    }

    test("consent cleanup clears saved TCP and restores owned value while gate is closed") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.apply("bbr", manager.readState()).successful)
        check(manager.setApplyOnBoot(true, manager.readState()).successful)
        port.ready = false
        check(manager.disableForConsent())
        check(port.current == "cubic" && port.bootAlgorithm == "off")
    }
    test("consent cleanup preserves another controller's TCP selection") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.apply("bbr", manager.readState()).successful)
        port.current = "westwood"
        val writes = port.writes.size
        port.ready = false
        check(manager.disableForConsent())
        check(port.current == "westwood" && port.writes.size == writes)
    }
    test("restore then acceptance cannot replay the old live command") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.apply("bbr", manager.readState()).successful)
        check(port.liveRequest == "bbr")
        port.ready = false
        check(manager.disableForConsent())
        check(port.current == "cubic" && port.liveRequest == "off" && port.restoreRequest == "off")
        val writes = port.writes.size
        port.consentChanged(true)
        check(port.current == "cubic" && port.writes.size == writes)
        // Another controller changes TCP before the next terms revocation.
        port.current = "westwood"
        port.consentChanged(false)
        check(port.current == "westwood" && port.writes.size == writes)
    }
    test("failed TCP reset cannot leave a command to replay at the next consent edge") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.apply("bbr", manager.readState()).successful)
        port.ready = false
        port.action = { }
        check(!manager.disableForConsent())
        check(port.liveRequest == "off" && port.restoreRequest == "off")
    }
    test("unconfirmed command neutralization reports reset failure") {
        val port = Port().apply { clearSucceeds = false; liveRequest = "bbr" }
        val manager = TcpControlManager(port, pause = {})
        check(!manager.disableForConsent())
        check(port.bootAlgorithm == "off" && port.writes.isEmpty())
    }
    test("runtime intersection excludes unsupported algorithms and preserves unknown current") {
        val port = Port().apply { current = "custom" }
        val manager = TcpControlManager(port, pause = {})
        val state = manager.readState()
        check(state.current == "custom" && state.initial == "custom")
        check(state.available == listOf("reno", "cubic", "westwood", "bbr", "bbr3"))
        check(state.canApply && !state.canRestore)
        check(manager.apply("bbr3", state).successful)
        check(!manager.readState().canRestore)
    }
    test("reject unknown, injection, empty, whitespace and runtime-absent selections") {
        val port = Port().apply { bootAlgorithm = "cubic" }
        val manager = TcpControlManager(port, pause = {})
        val state = manager.readState()
        listOf("custom", "bbr3\nreno", "cubic; reboot", "", " bbr3", "htcp").forEach {
            checkFailure(manager.apply(it, state), TcpFailure.INVALID_ALGORITHM)
        }
        check(port.writes.isEmpty() && port.bootSaves.isEmpty())
    }
    test("missing init integration does not capture initial before readiness") {
        val port = Port().apply { ready = false }
        val manager = TcpControlManager(port, pause = {})
        val state = manager.readState()
        check(state.current == "cubic" && state.initial == null && !state.canApply)
        checkFailure(manager.apply("bbr3", state), TcpFailure.UNAVAILABLE)
        port.ready = true
        check(manager.readState().initial == "cubic")
        port.readable = false
        check(manager.readState().current == null && manager.readState().initial == "cubic")
        check(port.writes.isEmpty())
    }
    test("initial is captured after init restores the boot choice and signals readiness") {
        val port = Port().apply {
            bootAlgorithm = "bbr3"
            ready = false
        }
        val manager = TcpControlManager(port, pause = {})
        val waiting = manager.readState()
        check(waiting.current == "cubic" && waiting.initial == null && !waiting.canApply)
        port.current = "bbr3"
        check(manager.readState().initial == null)
        port.ready = true
        val ready = manager.readState()
        check(ready.initial == "bbr3" && ready.current == "bbr3" && ready.canApply)
        check(!ready.canRestore && ready.bootAlgorithm == "bbr3")
        check(port.writes.isEmpty() && port.bootSaves.isEmpty())
    }
    test("malformed or empty runtime catalogue disables controls") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        listOf("", "cubic\nbbr3;bad", "bbr3 "+"x".repeat(1024)).forEach {
            port.available = it
            check(!manager.readState().canApply)
        }
    }
    test("successful readback enables explicit restore of process initial") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        val state = manager.readState()
        check(manager.apply("bbr3", state).successful)
        check(manager.readState().canRestore)
        check(manager.apply("westwood", manager.readState()).successful)
        check(manager.readState().initial == "cubic")
        check(manager.restoreInitial(manager.readState()).successful)
        check(port.current == "cubic" && !manager.readState().canRestore)
        check(port.writes == listOf("bbr3", "westwood", "cubic"))
    }
    test("selecting already active value is a no-op and does not claim ownership") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.apply("cubic", manager.readState()).successful)
        check(port.writes.isEmpty() && !manager.readState().canRestore)
        checkFailure(manager.restoreInitial(manager.readState()), TcpFailure.NOT_OWNED)
    }
    test("stale UI does not overwrite an externally changed value") {
        val port = Port().apply { bootAlgorithm = "cubic" }
        val manager = TcpControlManager(port, pause = {})
        val stale = manager.readState()
        port.current = "westwood"
        checkFailure(manager.apply("bbr3", stale), TcpFailure.CONFLICT)
        check(port.writes.isEmpty() && port.current == "westwood")
        check(port.bootSaves.isEmpty() && port.bootAlgorithm == "cubic")
    }
    test("runtime catalogue changes invalidate a queued selection") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        val stale = manager.readState()
        port.available += " htcp"
        checkFailure(manager.apply("bbr3", stale), TcpFailure.CONFLICT)
        check(port.writes.isEmpty())
    }
    test("property rejection is reported without claiming success") {
        val port = Port().apply {
            bootAlgorithm = "cubic"
            action = { throw SecurityException("denied") }
        }
        val manager = TcpControlManager(port, pause = {})
        val result = manager.apply("bbr3", manager.readState())
        checkFailure(result, TcpFailure.WRITE_FAILED)
        check(result.writeAttempted && result.state.current == "cubic")
        check(port.bootSaves.isEmpty() && result.state.bootAlgorithm == "cubic")
    }
    test("accepted property without kernel readback times out after one second") {
        val port = Port().apply {
            bootAlgorithm = "cubic"
            action = {}
        }
        var waited = 0L
        val manager = TcpControlManager(port, pause = { waited += it })
        val result = manager.apply("bbr3", manager.readState())
        checkFailure(result, TcpFailure.READBACK_FAILED)
        check(waited == 1000L && port.writes == listOf("bbr3"))
        check(result.state.current == "cubic" && !result.state.canRestore)
        check(port.bootSaves.isEmpty() && result.state.bootAlgorithm == "cubic")
    }
    test("delayed init succeeds only after actual stable readback") {
        val port = Port().apply { action = {} }
        var pauses = 0
        val manager = TcpControlManager(port, pause = {
            if (++pauses == 3) port.current = "bbr3"
        })
        check(manager.apply("bbr3", manager.readState()).successful)
        check(pauses == 4 && manager.readState().canRestore)
    }
    test("concurrent external change during verification is never rolled back") {
        val port = Port().apply { bootAlgorithm = "cubic" }
        val manager = TcpControlManager(port, pause = { port.current = "westwood" })
        val result = manager.apply("bbr3", manager.readState())
        checkFailure(result, TcpFailure.CONFLICT)
        check(port.current == "westwood" && port.writes == listOf("bbr3"))
        check(!manager.readState().canRestore)
        check(port.bootSaves.isEmpty() && result.state.bootAlgorithm == "cubic")
    }
    test("replacement with original value also cancels ownership") {
        val port = Port()
        val manager = TcpControlManager(port, pause = { port.current = "cubic" })
        checkFailure(manager.apply("bbr3", manager.readState()), TcpFailure.CONFLICT)
        check(!manager.readState().canRestore)
    }
    test("external update after success blocks restore even from a refreshed screen") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.apply("bbr3", manager.readState()).successful)
        val stale = manager.readState()
        port.current = "westwood"
        checkFailure(manager.restoreInitial(stale), TcpFailure.CONFLICT)
        checkFailure(manager.restoreInitial(manager.readState()), TcpFailure.NOT_OWNED)
        check(port.current == "westwood" && port.writes == listOf("bbr3"))
    }
    test("reading does not write or restore after controller recreation") {
        val port = Port()
        val first = TcpControlManager(port, pause = {})
        check(first.setApplyOnBoot(true, first.readState()).successful)
        check(first.apply("bbr3", first.readState()).successful)
        val recreated = TcpControlManager(port, pause = {})
        val state = recreated.readState()
        check(state.initial == "bbr3" && !state.canRestore && state.bootAlgorithm == "bbr3")
        check(recreated.readState() == state)
        check(port.writes == listOf("bbr3"))
        check(port.bootSaves == listOf("cubic", "bbr3"))
    }
    test("boot persistence is opt-in and runtime changes do not enable it") {
        listOf("", "off").forEach { stored ->
            val port = Port().apply { bootAlgorithm = stored }
            val manager = TcpControlManager(port, pause = {})
            val state = manager.readState()
            check(state.bootAlgorithm == null)
            check(port.writes.isEmpty() && port.bootSaves.isEmpty())
            val result = manager.apply("bbr3", state)
            check(result.successful && result.state.bootAlgorithm == null)
            check(port.current == "bbr3" && port.bootAlgorithm == stored)
            check(port.writes == listOf("bbr3") && port.bootSaves.isEmpty())
        }
    }
    test("enabling boot persistence captures current without a TCP request") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.apply("bbr3", manager.readState()).successful)
        val result = manager.setApplyOnBoot(true, manager.readState())
        check(result.successful && !result.writeAttempted)
        check(result.state.current == "bbr3" && result.state.initial == "cubic")
        check(result.state.bootAlgorithm == "bbr3" && port.bootAlgorithm == "bbr3")
        check(port.writes == listOf("bbr3") && port.bootSaves == listOf("bbr3"))
    }
    test("disabling boot persistence works when unavailable and leaves current unchanged") {
        listOf(true, false).forEach { readerAvailable ->
            val port = Port().apply { bootAlgorithm = "cubic" }
            val manager = TcpControlManager(port, pause = {})
            val stale = manager.readState()
            port.current = "westwood"
            port.ready = false
            port.readable = readerAvailable
            val result = manager.setApplyOnBoot(false, stale)
            check(result.successful && !result.writeAttempted && !result.state.canApply)
            check(result.state.current == if (readerAvailable) "westwood" else null)
            check(result.state.bootAlgorithm == null && port.bootAlgorithm == "off")
            check(port.current == "westwood" && port.writes.isEmpty())
            check(port.bootSaves == listOf("off"))
        }
    }
    test("successful applies and restore update an enabled boot preference") {
        val port = Port()
        val manager = TcpControlManager(port, pause = {})
        check(manager.setApplyOnBoot(true, manager.readState()).successful)
        listOf("bbr3", "westwood").forEach { target ->
            val result = manager.apply(target, manager.readState())
            check(result.successful && result.writeAttempted)
            check(result.state.current == target && result.state.bootAlgorithm == target)
        }
        val restored = manager.restoreInitial(manager.readState())
        check(restored.successful && restored.writeAttempted)
        check(restored.state.current == "cubic" && restored.state.bootAlgorithm == "cubic")
        check(!restored.state.canRestore)
        check(port.writes == listOf("bbr3", "westwood", "cubic"))
        check(port.bootSaves == listOf("cubic", "bbr3", "westwood", "cubic"))
    }
    test("persistence exceptions and readback mismatches retain successful runtime changes") {
        listOf(true, false).forEach { throws ->
            val port = Port().apply {
                bootAlgorithm = "cubic"
                saveAction = { if (throws) throw SecurityException("denied") }
            }
            val manager = TcpControlManager(port, pause = {})
            val result = manager.apply("bbr3", manager.readState())
            checkFailure(result, TcpFailure.PERSIST_FAILED)
            check(result.writeAttempted && result.state.current == "bbr3")
            check(result.state.canRestore && result.state.bootAlgorithm == "cubic")
            check(port.current == "bbr3" && port.bootAlgorithm == "cubic")
            check(port.writes == listOf("bbr3") && port.bootSaves == listOf("bbr3"))
        }
    }
    test("enabling boot persistence rejects stale current and catalogue without saving") {
        listOf(true, false).forEach { changeCurrent ->
            val port = Port()
            val manager = TcpControlManager(port, pause = {})
            val stale = manager.readState()
            if (changeCurrent) port.current = "westwood" else port.available += " htcp"
            val result = manager.setApplyOnBoot(true, stale)
            checkFailure(result, TcpFailure.CONFLICT)
            check(!result.writeAttempted && result.state.bootAlgorithm == null)
            check(port.writes.isEmpty() && port.bootSaves.isEmpty())
        }
    }
    test("enabling boot persistence rejects unsupported or runtime-absent current values") {
        listOf("custom", "htcp").forEach { current ->
            val port = Port().apply { this.current = current }
            val manager = TcpControlManager(port, pause = {})
            val state = manager.readState()
            check(state.canApply)
            checkFailure(manager.setApplyOnBoot(true, state), TcpFailure.INVALID_ALGORITHM)
            check(port.current == current && port.bootAlgorithm == "off")
            check(port.writes.isEmpty() && port.bootSaves.isEmpty())
        }
    }
    test("enabling boot persistence requires available kernel control") {
        val port = Port().apply { ready = false }
        val manager = TcpControlManager(port, pause = {})
        checkFailure(manager.setApplyOnBoot(true, manager.readState()), TcpFailure.UNAVAILABLE)
        check(port.writes.isEmpty() && port.bootSaves.isEmpty())
    }
    test("concurrent callers serialize and the stale second caller cannot overwrite") {
        val port = Port()
        val pending = CountDownLatch(1)
        val proceed = CountDownLatch(1)
        val manager = TcpControlManager(port, pause = {
            pending.countDown()
            proceed.await()
        })
        val before = manager.readState()
        var first: TcpResult? = null
        var second: TcpResult? = null
        val one = thread { first = manager.apply("bbr3", before) }
        pending.await()
        val two = thread { second = manager.apply("westwood", before) }
        proceed.countDown()
        one.join()
        two.join()
        check(first?.successful == true)
        checkFailure(checkNotNull(second), TcpFailure.CONFLICT)
        check(port.writes == listOf("bbr3"))
    }
    println("$cases TCP control tests passed")
}
