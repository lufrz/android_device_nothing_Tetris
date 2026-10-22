/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.battery

import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread
import org.lineageos.settings.tetris.consent.PartsControlGate

private fun status(requested: Boolean = false, online: Boolean = true, active: Boolean = false,
    powerPath: Boolean = true, charging: Boolean = !active, error: Int = 0): String =
    "version=1 requested=${if (requested) 1 else 0} online=${if (online) 1 else 0} " +
        "active=${if (active) 1 else 0} power_path=${if (powerPath) 1 else 0} " +
        "charging=${if (charging) 1 else 0} error=$error\n"

private class Port : ChargeBypassPort {
    var present = true
    var canWrite = true
    var request = "0\n"
    var report = status()
    var readable = true
    var statusReadable = true
    val writes = mutableListOf<Boolean>()
    var write: (Boolean) -> Unit = {
        request = if (it) "1\n" else "0\n"
        report = status(requested = it, active = it)
    }
    override fun exists() = present
    override fun writable() = canWrite
    override fun readRequested(): String = if (readable) request else throw IOException("request unavailable")
    override fun readStatus(): String = if (statusReadable) report else throw IOException("status unavailable")
    override fun writeRequested(requested: Boolean) { writes += requested; write(requested) }
}

fun main() {
    var count = 0
    fun test(name: String, block: () -> Unit) {
        PartsControlGate.allowed = true
        block()
        println("PASS $name")
        count++
    }
    fun failure(result: ChargeBypassResult, expected: ChargeBypassFailure) {
        check(result.failure == expected) { "Expected $expected: $result" }
    }

    test("parser accepts inactive, verified active and policy-paused samples") {
        check(ChargeBypassStatus.parse(status())?.active == false)
        check(ChargeBypassStatus.parse(status(requested = true, active = true))?.active == true)
        val paused = ChargeBypassStatus.parse(status(requested = true, powerPath = false, charging = false))!!
        check(!paused.active && paused.requested && !paused.powerPath)
        check(ChargeBypassStatus.parse(status(requested = true, error = -5))?.error == -5)
    }
    test("parser rejects unsupported versions, missing duplicate or unknown fields and malformed values") {
        val valid = status()
        listOf("", "x".repeat(513), valid.replace("version=1", "version=2"),
            valid.replace("online=1 ", ""), valid.trim()+" online=1", valid.trim()+" surprise=1",
            valid.replace("online=1", "online=true"), valid.replace("requested=0", "requested=2"),
            valid.replace("error=0", "error=+5"), valid.replace("error=0", "error=-4096"),
            valid.replace("error=0", "error=999999999999999999"),
            valid.replace(" charging=1", "\ncharging=1"), "version=1=1").forEach {
            check(ChargeBypassStatus.parse(it) == null) { "Accepted malformed status: $it" }
        }
    }
    test("active requires every verified safety condition") {
        listOf(status(active = true), status(requested = true, active = true, online = false),
            status(requested = true, active = true, powerPath = false),
            status(requested = true, active = true, charging = true),
            status(requested = true, active = true, error = -5)).forEach {
            check(ChargeBypassStatus.parse(it) == null)
        }
    }
    test("read-only polling never changes a request") {
        val port = Port()
        val manager = ChargeBypassManager(port)
        repeat(3) { check(manager.readState().canEnable) }
        check(port.writes.isEmpty())
    }
    test("enable requires online, writable, supported and mutually consistent readings") {
        val ports = listOf(Port().apply { report = status(online = false) },
            Port().apply { canWrite = false }, Port().apply { report = "version=2" },
            Port().apply { request = "not a bit" }, Port().apply { readable = false },
            Port().apply { report = status(requested = true) })
        ports.forEach { port ->
            val manager = ChargeBypassManager(port)
            check(!manager.readState().canEnable)
            failure(manager.setRequested(true), ChargeBypassFailure.UNAVAILABLE)
            check(port.writes.isEmpty())
        }
    }
    test("enable rechecks connection after the UI snapshot") {
        val port = Port()
        val manager = ChargeBypassManager(port)
        check(manager.readState().canEnable)
        port.report = status(online = false)
        failure(manager.setRequested(true), ChargeBypassFailure.UNAVAILABLE)
        check(port.writes.isEmpty())
    }
    test("enable verifies request and activation separately") {
        val port = Port().apply { write = { request = "1"; report = status(requested = true, powerPath = false) } }
        val result = ChargeBypassManager(port).setRequested(true)
        check(result.successful && result.state.requested == true && result.state.status?.active == false)
        check(result.state.canStop && port.writes == listOf(true))
    }
    test("duplicate enable acknowledges the existing request without rewriting it") {
        val port = Port()
        val manager = ChargeBypassManager(port)
        check(manager.setRequested(true).successful)
        check(manager.setRequested(true).successful)
        check(port.writes == listOf(true))
    }
    test("unacknowledged enable fails and does not pretend to be active") {
        val port = Port().apply { write = {} }
        val result = ChargeBypassManager(port).setRequested(true)
        failure(result, ChargeBypassFailure.READBACK_FAILED)
        check(!result.state.canStop && result.state.status?.active == false)
    }
    test("enable that throws after acceptance retains Stop and shows the controller error") {
        val port = Port().apply {
            write = { request = "1"; report = status(requested = true, error = -5); throw IOException("hardware error") }
        }
        val manager = ChargeBypassManager(port)
        val result = manager.setRequested(true)
        failure(result, ChargeBypassFailure.WRITE_FAILED)
        check(result.state.canStop && result.state.status?.error == -5)
        port.write = { port.request = "0"; port.report = status() }
        check(manager.setRequested(false).successful)
        check(port.writes == listOf(true, false))
    }
    test("lost readback after enable preserves release action until verified off") {
        val port = Port().apply { write = { readable = false; statusReadable = false } }
        val manager = ChargeBypassManager(port)
        val result = manager.setRequested(true)
        failure(result, ChargeBypassFailure.READBACK_FAILED)
        check(result.state.canStop && result.state.status == null)
        port.write = { port.readable = true; port.request = "0" }
        check(manager.setRequested(false).successful)
        check(!manager.readState().canStop)
    }
    test("missing telemetry or offline state never prevents release") {
        listOf(true, false).forEach { missing ->
            val port = Port().apply {
                request = "1"
                report = if (missing) "invalid" else status(requested = true, online = false)
                canWrite = false // A failed access probe must not suppress an explicit release attempt.
            }
            val manager = ChargeBypassManager(port)
            check(manager.readState().canStop)
            check(manager.setRequested(false).successful)
            check(port.writes == listOf(false))
        }
    }
    test("failed release retains the request and reports failure") {
        val port = Port().apply { request = "1"; report = status(requested = true); write = {} }
        val manager = ChargeBypassManager(port)
        failure(manager.setRequested(false), ChargeBypassFailure.READBACK_FAILED)
        check(manager.readState().canStop)
        port.write = { throw IOException("denied") }
        failure(manager.setRequested(false), ChargeBypassFailure.WRITE_FAILED)
        check(!manager.disableForConsent())
    }
    test("consent denies enable but never blocks release of the dedicated request") {
        val port = Port()
        val manager = ChargeBypassManager(port)
        PartsControlGate.allowed = false
        failure(manager.setRequested(true), ChargeBypassFailure.CONSENT_REQUIRED)
        check(port.writes.isEmpty())
        port.request = "1"; port.report = status(requested = true)
        check(manager.disableForConsent())
        check(port.writes == listOf(false))
    }
    test("consent cleanup on unsupported kernels is harmless") {
        val port = Port().apply { present = false; readable = false; statusReadable = false }
        check(ChargeBypassManager(port).disableForConsent())
        check(port.writes.isEmpty())
    }
    test("consent cleanup attempts release with unreadable existing controls") {
        val port = Port().apply { readable = false; statusReadable = false
            write = { readable = true; request = "0" } }
        check(ChargeBypassManager(port).disableForConsent())
        check(port.writes == listOf(false))
    }
    test("external request and kernel unplug reset are observed without automatic reapply") {
        val port = Port()
        val manager = ChargeBypassManager(port)
        port.request = "1"; port.report = status(requested = true, active = true)
        check(manager.readState().canStop)
        port.request = "0"; port.report = status(online = false)
        check(!manager.readState().canStop && !manager.readState().canEnable)
        port.report = status()
        check(manager.readState().canEnable)
        check(port.writes.isEmpty())
    }
    test("recreated manager never re-enables a previous session and can release the current vote") {
        val port = Port()
        check(ChargeBypassManager(port).setRequested(true).successful)
        val recreated = ChargeBypassManager(port)
        check(recreated.readState().canStop && port.writes == listOf(true))
        PartsControlGate.allowed = false
        check(recreated.disableForConsent())
        check(port.writes == listOf(true, false))
    }
    test("consent revocation drains an in-flight enable before releasing it") {
        val entered = CountDownLatch(1)
        val proceed = CountDownLatch(1)
        val cleaned = CountDownLatch(1)
        val port = Port().apply { write = {
            if (it) { entered.countDown(); check(proceed.await(3, TimeUnit.SECONDS)) }
            request = if (it) "1" else "0"; report = status(requested = it, active = it)
        } }
        val manager = ChargeBypassManager(port)
        val enabling = thread { check(manager.setRequested(true).successful) }
        check(entered.await(3, TimeUnit.SECONDS))
        PartsControlGate.allowed = false
        val reset = thread {
            PartsControlGate.awaitPendingMutations()
            check(manager.disableForConsent())
            cleaned.countDown()
        }
        check(!cleaned.await(50, TimeUnit.MILLISECONDS))
        proceed.countDown()
        enabling.join(3000); reset.join(3000)
        check(!enabling.isAlive && !reset.isAlive && cleaned.count == 0L)
        check(port.writes == listOf(true, false) && !manager.readState().canStop)
    }
    println("$count charge suspension tests passed")
}
