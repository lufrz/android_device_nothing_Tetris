// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent
import android.content.Context
import android.os.SystemProperties
import android.os.UserHandle
import org.lineageos.settings.tetris.network.TcpControlSession
private var cases = 0
private fun test(name: String, body: (Context)->Unit) {
    SystemProperties.values.clear(); SystemProperties.fail = false; UserHandle.user = 0
    PartsControlGate.allowed = false
    TcpControlSession.clearCount = 0; TcpControlSession.clearSucceeds = true
    try { body(Context()) } catch (error: Throwable) { throw AssertionError(name, error) }
    cases++
}
fun main() {
    test("fresh install requires manual consent without launching automatically") { c ->
        check(!TermsConsent.synchronize(c)); check(!TermsConsent.isAccepted(c))
        check(!TermsConsent.shouldPromptAutomatically(c))
        check(c.prefs.values["identity_version"] == 1)
        check(c.prefs.values["presented_epoch"] == "terms:1/request:0")
    }
    test("reinstall requires manual consent without launching automatically") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), true))
        c.prefs.values.clear()
        check(!TermsConsent.synchronize(c)); check(!TermsConsent.isAccepted(c))
        check(!TermsConsent.shouldPromptAutomatically(c))
        check(!PartsControlGate.allowed)
    }
    test("identity names terms revision and explicit request generation") { c ->
        check(TermsConsent.epoch(c) == "terms:1/request:0")
    }
    test("legacy accepted terms migrate once without asking again") { c ->
        val previous = "a".repeat(64)
        c.prefs.values["accepted_epoch"] = previous
        c.prefs.values["presented_epoch"] = previous
        check(TermsConsent.synchronize(c)); check(TermsConsent.isAccepted(c))
        check(!TermsConsent.shouldPromptAutomatically(c))
        check(c.prefs.values["identity_version"] == 1)
        check(c.prefs.values["accepted_epoch"] == "terms:1/request:0")
        check(c.prefs.values["presented_epoch"] == "terms:1/request:0")
    }
    test("legacy refusal or dismissed window stays closed without reappearing") { c ->
        c.prefs.values["presented_epoch"] = "b".repeat(64)
        check(!TermsConsent.synchronize(c)); check(!TermsConsent.isAccepted(c))
        check(!TermsConsent.shouldPromptAutomatically(c))
        check((c.prefs.values["accepted_epoch"] as? String).orEmpty().isEmpty())
        check(c.prefs.values["presented_epoch"] == "terms:1/request:0")
    }
    test("legacy accepted state can still be explicitly declined after migration") { c ->
        val previous = "c".repeat(64)
        c.prefs.values["accepted_epoch"] = previous
        c.prefs.values["presented_epoch"] = previous
        check(TermsConsent.synchronize(c))
        check(TermsConsent.decide(c, TermsConsent.epoch(c), false))
        check(!TermsConsent.isAccepted(c)); check(!TermsConsent.shouldPromptAutomatically(c))
        check(!PartsControlGate.allowed)
    }
    test("malformed legacy identifiers never grant acceptance") { c ->
        for (identifier in listOf("yes", "f".repeat(63), "g".repeat(64), "A".repeat(64))) {
            c.prefs.values.clear()
            c.prefs.values["accepted_epoch"] = identifier
            c.prefs.values["presented_epoch"] = identifier
            check(!TermsConsent.isAccepted(c))
        }
    }
    test("legacy migration cannot run again after its marker was saved") { c ->
        c.prefs.values["identity_version"] = 1
        c.prefs.values["accepted_epoch"] = "d".repeat(64)
        c.prefs.values["presented_epoch"] = "d".repeat(64)
        check(!TermsConsent.isAccepted(c))
    }
    test("failed legacy migration cannot enable features") { c ->
        c.prefs.values["accepted_epoch"] = "e".repeat(64)
        c.prefs.values["presented_epoch"] = "e".repeat(64)
        c.prefs.writable = false
        check(!TermsConsent.synchronize(c)); check(!TermsConsent.isAccepted(c))
        check(!PartsControlGate.allowed)
    }
    test("legacy consent is never promoted to changed terms or an explicit request") {
        val previous = "f".repeat(64)
        for (next in listOf("terms:2/request:0", "terms:1/request:1")) {
            val migrated = TermsState(previous, previous).migrateLegacy(next)
            check(migrated.accepted == "terms:1/request:0")
            check(!migrated.permits(next)); check(migrated.needsPresentation(next))
        }
    }
    test("legacy refusal must also be presented for changed terms") {
        val next = "terms:2/request:0"
        val migrated = TermsState(presented = "a".repeat(64)).migrateLegacy(next)
        check(migrated.accepted.isEmpty())
        check(!migrated.permits(next)); check(migrated.needsPresentation(next))
    }
    test("ordinary reboot keeps acceptance but republishes volatile gate") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), true))
        SystemProperties.values.remove("sys.tetrisparts.consent")
        check(TermsConsent.synchronize(c)); check(!TermsConsent.shouldPromptAutomatically(c))
        check(SystemProperties.get("sys.tetrisparts.consent", "0") == "1")
    }
    test("declining disables Parts and never repeats on an ordinary reboot") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), false))
        check(!TermsConsent.synchronize(c)); check(!TermsConsent.shouldPromptAutomatically(c))
        check(!TermsConsent.isAccepted(c))
    }
    test("shown without choice does not repeatedly prompt on reboot") { c ->
        check(TermsConsent.markPresented(c))
        check(!TermsConsent.shouldPromptAutomatically(c)); check(!TermsConsent.isAccepted(c))
    }
    test("ROM build and vendor identity changes preserve acceptance") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), true))
        SystemProperties.values["ro.system.build.date.utc"] = "2000"
        SystemProperties.values["ro.build.date.utc"] = "3000"
        SystemProperties.values["ro.build.fingerprint"] = "another-build"
        check(TermsConsent.synchronize(c)); check(!TermsConsent.shouldPromptAutomatically(c))
        check(SystemProperties.get("sys.tetrisparts.consent", "0") == "1")
    }
    test("APK replacement preserves a previous refusal") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), false))
        c.packageManager.info.lastUpdateTime++
        check(!TermsConsent.isAccepted(c)); check(!TermsConsent.shouldPromptAutomatically(c))
    }
    test("APK version change preserves acceptance") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), true))
        c.packageManager.info.longVersionCode++
        check(TermsConsent.isAccepted(c)); check(!TermsConsent.shouldPromptAutomatically(c))
    }
    test("ordinary APK replacement does not invalidate an open consent window") { c ->
        val displayed = TermsConsent.epoch(c)
        c.packageManager.info.lastUpdateTime++
        check(TermsConsent.decide(c, displayed, true)); check(TermsConsent.isAccepted(c))
    }
    test("changed terms require a new decision and one automatic presentation") { c ->
        c.prefs.values["identity_version"] = 1
        c.prefs.values["accepted_epoch"] = "terms:0/request:0"
        c.prefs.values["presented_epoch"] = "terms:0/request:0"
        check(!TermsConsent.synchronize(c)); check(TermsConsent.shouldPromptAutomatically(c))
        check(TermsConsent.markPresented(c))
        check(!TermsConsent.shouldPromptAutomatically(c)); check(!TermsConsent.isAccepted(c))
    }
    test("explicit request generation requires a new decision") {
        val next = "terms:1/request:1"
        val saved = TermsState("terms:1/request:0", "terms:1/request:0")
        check(!saved.permits(next)); check(saved.needsPresentation(next))
        val presented = saved.shown(next)
        check(!presented.permits(next)); check(!presented.needsPresentation(next))
        val accepted = presented.decide(next, true)
        check(accepted.permits(next)); check(!accepted.needsPresentation(next))
    }
    test("windows for different terms or requests cannot approve the current identity") { c ->
        for (old in listOf("terms:0/request:0", "terms:1/request:1")) {
            check(!TermsConsent.decide(c, old, true)); check(!TermsConsent.isAccepted(c))
        }
    }
    test("failed durable save does not enable features") { c ->
        c.prefs.writable = false
        check(!TermsConsent.decide(c, TermsConsent.epoch(c), true)); check(!TermsConsent.isAccepted(c))
    }
    test("failed gate publication rolls acceptance back") { c ->
        SystemProperties.fail = true
        check(!TermsConsent.decide(c, TermsConsent.epoch(c), true)); check(!TermsConsent.isAccepted(c))
    }
    test("pending commands are neutralized before opening gate") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), true))
        check(TcpControlSession.clearCount == 1 && PartsControlGate.allowed)
        check(TermsConsent.synchronize(c)); check(TcpControlSession.clearCount == 1)
    }
    test("unneutralized network request cannot be replayed by accepting") { c ->
        TcpControlSession.clearSucceeds = false
        check(!TermsConsent.decide(c, TermsConsent.epoch(c), true))
        check(!TermsConsent.isAccepted(c) && !PartsControlGate.allowed)
        check(SystemProperties.get("sys.tetrisparts.consent", "0") == "0")
    }
    test("network cleanup failure cannot keep audio enabled after terms invalidation") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), true))
        c.prefs.values["accepted_epoch"] = "terms:0/request:0"
        c.prefs.values["presented_epoch"] = "terms:0/request:0"
        TcpControlSession.clearSucceeds = false
        check(!TermsConsent.synchronize(c)); check(!PartsControlGate.allowed)
        check(SystemProperties.get("sys.tetrisparts.consent", "1") == "0")
    }
    test("secondary users cannot alter owner's global consent") { c ->
        check(TermsConsent.decide(c, TermsConsent.epoch(c), true))
        UserHandle.user = 10
        check(!TermsConsent.synchronize(c)); check(!TermsConsent.isAccepted(c))
        check(!TermsConsent.shouldPromptAutomatically(c)); check(!TermsConsent.decide(c, TermsConsent.epoch(c), false))
        check(SystemProperties.get("sys.tetrisparts.consent", "0") == "1")
    }
    test("20 seconds and ceil countdown") {
        val deadline = TermsCountdown.deadline(1000L, null)
        check(deadline == 21000L)
        for (elapsed in 0L..20000L) {
            check(TermsCountdown.remaining(deadline, 1000L+elapsed) == ((20000L-elapsed+999L)/1000L).toInt())
        }
        check(TermsCountdown.remaining(deadline, 22000L) == 0)
    }
    test("rotation keeps elapsed wait including after expiry") {
        check(TermsCountdown.deadline(11000L, 21000L) == 21000L)
        check(TermsCountdown.deadline(22000L, 21000L) == 21000L)
        check(TermsCountdown.remaining(21000L, 22000L) == 0)
    }
    test("invalid future or negative saved deadline starts a fresh wait") {
        check(TermsCountdown.deadline(1000L, 80000L) == 21000L)
        check(TermsCountdown.deadline(1000L, -1L) == 21000L)
    }
    println("Terms identity, storage, fail-closed gate and countdown: $cases scenarios PASS")
}
