// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent

import android.app.StatusBarManager
import android.os.Bundle
import android.os.SystemClock
import android.os.UserHandle
import android.view.WindowInsets
import android.widget.Toast
import org.lineageos.settings.tetris.PartsActivity
import java.util.concurrent.CountDownLatch

private var checks = 0
private var scenarios = 0
private val activities = mutableListOf<TermsActivity>()

private fun expect(value: Boolean, message: String) {
    checks++
    check(value) { message }
}

private fun eventually(message: String, condition: () -> Boolean) {
    val end = System.nanoTime() + 5_000_000_000L
    while (!condition()) {
        check(System.nanoTime() < end) { "Timed out: $message" }
        Thread.sleep(5)
    }
}

private fun scenario(name: String, test: () -> Unit) {
    TermsConsent.reset()
    PartsFeatureReset.reset()
    TermsLifecycle.cancelCount = 0
    SystemClock.now = 1000
    UserHandle.user = UserHandle.USER_SYSTEM
    Toast.shown = 0
    Screen.current = null
    try {
        test()
        scenarios++
    } catch (failure: Throwable) {
        throw AssertionError(name, failure)
    } finally {
        PartsFeatureReset.gate?.countDown()
        TermsConsent.decisionGate?.countDown()
        activities.forEach { it.onDestroy() }
        activities.clear()
    }
}

private fun activity(mandatory: Boolean, saved: Bundle? = null): TermsActivity =
    TermsActivity().also {
        activities += it
        it.intent = TermsActivity.intent(it, mandatory)
        it.onCreate(saved)
    }

private fun TermsActivity.screen(): ScreenState {
    render()
    return checkNotNull(Screen.current)
}

private fun TermsActivity.ready() {
    eventually("feature cleanup completes") { screen().let { !it.preparing && !it.busy } }
}

private fun TermsActivity.requireReleased() {
    expect(barManager.primary.lastOrNull() == StatusBarManager.DISABLE_NONE,
        "Primary restrictions must be released")
    expect(barManager.secondary.lastOrNull() == StatusBarManager.DISABLE2_NONE,
        "Secondary restrictions must be released")
    expect(window.insetsController!!.shown.last() == WindowInsets.Type.navigationBars(),
        "Navigation bars must be restored")
}

fun main() {
    scenario("timer is enforced by callback, not only the disabled UI") {
        val a = activity(false)
        a.ready()
        expect(a.screen().remaining == 20, "Initial countdown must be 20 seconds")
        a.screen().accept()
        expect(TermsConsent.decisionCount == 0, "An early callback must not accept")
        SystemClock.now = 20_999
        a.screen().accept()
        expect(TermsConsent.decisionCount == 0, "Even one millisecond early must not accept")
        SystemClock.now = 21_000
        a.screen().accept()
        eventually("accepted at exact deadline") { a.isFinishing }
        expect(TermsConsent.lastDecision == ("terms-one" to true), "Accept must use displayed epoch")
        expect(a.launched.single().destination == PartsActivity::class.java,
            "Manual acceptance must open Parts")
    }
    scenario("rendered countdown updates and reaches zero") {
        val a = activity(false)
        a.ready()
        SystemClock.now = 3_501
        eventually("18 seconds displayed") { a.screen().remaining == 18 }
        SystemClock.now = 21_000
        eventually("zero displayed") { a.screen().remaining == 0 }
        expect(TermsConsent.decisionCount == 0, "Countdown expiration must not accept automatically")
    }
    scenario("decline has no countdown gate") {
        val a = activity(true)
        a.ready()
        a.onResume()
        expect(a.screen().remaining == 20, "Decline test must occur before countdown")
        a.screen().decline()
        eventually("decline finishes") { a.isFinishing }
        expect(TermsConsent.lastDecision == ("terms-one" to false), "Decline must persist refusal")
        expect(a.launched.isEmpty(), "Decline must not open Parts")
        a.requireReleased()
    }
    scenario("manual entry does not touch System UI restrictions") {
        val a = activity(false)
        a.ready()
        a.onResume()
        expect(!a.screen().mandatory, "Manual screen flag")
        expect(TermsConsent.markCount == 1, "Visible manual terms satisfy the pending presentation")
        expect(TermsLifecycle.cancelCount == 1, "Manual entry must cancel a pending mandatory prompt")
        expect(a.onBackPressedDispatcher.callbacks.none { it.isEnabled }, "Manual back must remain available")
        a.screen().back()
        a.onPause(); a.onStop(); a.onDestroy()
        expect(a.barManager.primary.isEmpty() && a.barManager.secondary.isEmpty(),
            "Manual entry must not disable system navigation or shade")
        expect(a.window.insetsController!!.hidden.isEmpty(), "Manual navigation bars must stay visible")
        expect(TermsConsent.decisionCount == 0, "Back is not consent")
    }
    scenario("mandatory restrictions apply only while visible") {
        val a = activity(true)
        a.ready()
        expect(a.barManager.primary.isEmpty(), "Creating an unseen window must not restrict UI")
        a.onResume()
        expect(a.screen().mandatory, "Mandatory screen flag")
        expect(TermsConsent.markCount == 1, "Visible automatic window must be recorded")
        expect(TermsLifecycle.cancelCount == 1, "Visible automatic window cancels obsolete prompt job")
        val flags = a.barManager.primary.single()
        val required = StatusBarManager.DISABLE_EXPAND or StatusBarManager.DISABLE_HOME or
            StatusBarManager.DISABLE_RECENT or StatusBarManager.DISABLE_BACK or StatusBarManager.DISABLE_SEARCH
        expect(flags == required, "Home, Back, Recents, search and shade must be restricted")
        expect(a.barManager.secondary.single() ==
            (StatusBarManager.DISABLE2_QUICK_SETTINGS or StatusBarManager.DISABLE2_NOTIFICATION_SHADE),
            "Quick settings and shade must be restricted")
        expect(a.window.insetsController!!.hidden.single() == WindowInsets.Type.navigationBars(),
            "Only navigation bars are hidden")
        expect(a.window.insetsController!!.shown.single() == WindowInsets.Type.statusBars(),
            "Status bar stays visible")
        expect(a.onBackPressedDispatcher.callbacks.single().isEnabled, "Mandatory Back intercepted")
        a.onBackPressedDispatcher.callbacks.single().handleOnBackPressed()
        expect(!a.isFinishing, "Back must not discard mandatory choice")
        a.onPause()
        a.requireReleased()
        val count = a.barManager.primary.size
        a.onStop(); a.onDestroy()
        expect(a.barManager.primary.size == count, "Release is idempotent across lifecycle callbacks")
    }
    scenario("onStop independently releases flags") {
        val a = activity(true); a.ready(); a.onResume(); a.onStop(); a.requireReleased()
    }
    scenario("onDestroy independently releases flags") {
        val a = activity(true); a.ready(); a.onResume(); a.onDestroy(); a.requireReleased()
    }
    scenario("resume reapplies released mandatory restrictions") {
        val a = activity(true); a.ready(); a.onResume(); a.onPause(); a.onResume()
        expect(a.barManager.primary.last() != 0, "Visible mandatory window must restrict UI again")
        a.onPause(); a.requireReleased()
    }
    scenario("partial status bar failure does not skip release of other flags") {
        val a = activity(true); a.ready()
        a.barManager.throwOnPrimary = true
        a.onResume(); a.onPause()
        a.requireReleased()
    }
    scenario("mandatory acceptance releases UI and does not open Parts") {
        val a = activity(true); a.ready(); a.onResume()
        SystemClock.now = 21_000
        a.screen().accept()
        eventually("mandatory accepted") { a.isFinishing }
        a.requireReleased()
        expect(a.launched.isEmpty(), "Setup/update acceptance should return to existing UI")
        expect(TermsLifecycle.cancelCount == 2, "Completed choice must cancel future prompt job")
    }
    scenario("rotation preserves remaining time instead of restarting") {
        val first = activity(true); first.ready(); first.onResume()
        SystemClock.now = 13_000
        val state = Bundle().also { first.onSaveInstanceState(it) }
        first.onPause(); first.onDestroy()
        val next = activity(true, state); next.ready(); next.onResume()
        expect(next.screen().remaining == 8, "Rotation must retain elapsed reading time")
        SystemClock.now = 21_000
        next.screen().accept()
        eventually("accept after original deadline") { next.isFinishing }
        expect(TermsConsent.decisionCount == 1, "Only the surviving window accepts")
    }
    scenario("different build discards saved countdown") {
        val first = activity(false); first.ready()
        SystemClock.now = 13_000
        val state = Bundle().also { first.onSaveInstanceState(it) }
        first.onDestroy()
        TermsConsent.currentEpoch = "terms-two"
        val next = activity(false, state); next.ready()
        expect(next.screen().remaining == 20, "An updated build requires a new reading countdown")
        SystemClock.now = 21_000
        next.screen().accept()
        expect(TermsConsent.decisionCount == 0, "Previous terms deadline must not enable acceptance")
    }
    scenario("a hanging reset cannot delay immediate refusal") {
        PartsFeatureReset.gate = CountDownLatch(1)
        val a = activity(true); a.onResume()
        eventually("reset started") { PartsFeatureReset.calls == 1 }
        expect(a.screen().preparing && !a.screen().busy,
            "Reset preparation must not block refusal")
        expect(a.screen().remaining == 20, "Refusal is tested immediately, before reading delay")
        a.screen().decline()
        eventually("refusal closes while reset is still blocked") { a.isFinishing }
        expect(PartsFeatureReset.gate!!.count == 1L, "Reset was not unblocked to permit refusal")
        expect(TermsConsent.lastDecision == ("terms-one" to false), "Immediate refusal must be saved")
        expect(!TermsConsent.accepted && a.launched.isEmpty(), "Refusal cannot enable or open Parts")
        a.requireReleased()
    }
    scenario("elapsed reading time cannot bypass pending feature cleanup") {
        PartsFeatureReset.gate = CountDownLatch(1)
        val a = activity(true); a.onResume()
        SystemClock.now = 21_000
        eventually("countdown reaches zero while preparing") { a.screen().remaining == 0 }
        expect(a.screen().preparing, "Reset must still be pending")
        a.screen().accept()
        expect(TermsConsent.decisionCount == 0, "Acceptance must wait for reset even after 20 seconds")
        PartsFeatureReset.gate!!.countDown(); a.ready()
        a.screen().accept()
        eventually("acceptance after reset") { a.isFinishing }
        expect(TermsConsent.lastDecision == ("terms-one" to true), "Completed reset enables acceptance")
        a.requireReleased()
    }
    scenario("cleanup failure cannot be accepted but permits decline") {
        PartsFeatureReset.failures = listOf("audio")
        val a = activity(true); a.ready(); a.onResume()
        expect(a.screen().error, "Failed cleanup must be reported")
        SystemClock.now = 21_000
        a.screen().accept()
        expect(TermsConsent.decisionCount == 0, "Failed reset cannot enable Parts")
        a.screen().decline()
        eventually("decline despite reset error") { a.isFinishing }
        a.requireReleased()
    }
    scenario("reset completion preserves failed presentation recording") {
        PartsFeatureReset.gate = CountDownLatch(1)
        TermsConsent.markSucceeds = false
        val a = activity(true); a.onResume()
        expect(a.screen().error, "Presentation persistence failure must be visible")
        PartsFeatureReset.gate!!.countDown(); a.ready()
        expect(a.screen().error, "Successful feature reset must not erase another failure")
        SystemClock.now = 21_000
        a.screen().accept()
        expect(TermsConsent.decisionCount == 0, "Unresolved presentation failure must keep acceptance disabled")
        a.screen().decline()
        eventually("decline despite presentation error") { a.isFinishing }
        a.requireReleased()
    }
    scenario("duplicate clicks are serialized during persistence") {
        TermsConsent.decisionGate = CountDownLatch(1)
        val a = activity(true); a.ready(); a.onResume()
        SystemClock.now = 21_000
        a.screen().accept()
        eventually("one persistence operation") { TermsConsent.decisionCount == 1 }
        a.screen().accept(); a.screen().decline()
        expect(TermsConsent.decisionCount == 1, "Busy decision must reject subsequent callbacks")
        expect(!a.isFinishing, "Window stays until decision finishes")
        TermsConsent.decisionGate!!.countDown()
        eventually("saved choice") { a.isFinishing }
        a.requireReleased()
    }
    scenario("failed acceptance remains unavailable and lets user decline") {
        TermsConsent.saveSucceeds = false
        val a = activity(true); a.ready(); a.onResume()
        SystemClock.now = 21_000
        a.screen().accept()
        eventually("save error presented") { !a.screen().busy && a.screen().error }
        expect(!a.isFinishing && !TermsConsent.accepted, "Failed acceptance must not enable Parts")
        a.screen().decline()
        eventually("failed refusal still exits") { a.isFinishing }
        expect(Toast.shown == 1, "Failure to record refusal must be disclosed")
        a.requireReleased()
    }
    scenario("failed refusal never traps user") {
        TermsConsent.saveSucceeds = false
        val a = activity(true); a.ready(); a.onResume(); a.screen().decline()
        eventually("failed refusal exits") { a.isFinishing }
        expect(!TermsConsent.accepted, "Failed refusal must not grant consent")
        expect(Toast.shown == 1, "Refusal persistence failure must be disclosed")
        a.requireReleased()
    }
    scenario("older displayed terms cannot be accepted after revision change") {
        val a = activity(false); a.ready()
        TermsConsent.currentEpoch = "terms-two"
        SystemClock.now = 21_000
        a.screen().accept()
        eventually("stale epoch rejected") { !a.screen().busy && a.screen().error }
        expect(TermsConsent.lastDecision?.first == "terms-one", "Persist must validate displayed epoch")
        expect(!a.isFinishing && !TermsConsent.accepted, "Stale window cannot enable Parts")
    }
    scenario("already accepted build exits without reset or restrictions") {
        TermsConsent.accepted = true
        val a = activity(true); a.onResume()
        expect(a.isFinishing, "Accepted build should not show terms")
        expect(a.content == null && PartsFeatureReset.calls == 0, "Accepted features must not be reset")
        expect(a.barManager.primary.isEmpty(), "Accepted build must not restrict UI")
    }
    scenario("secondary user cannot show or accept owner terms") {
        UserHandle.user = 10
        val a = activity(true); a.onResume()
        expect(a.isFinishing, "Secondary user must exit")
        expect(a.content == null && PartsFeatureReset.calls == 0, "Secondary user must not reset owner features")
        expect(a.barManager.primary.isEmpty(), "Secondary user must not restrict UI")
    }
    println("TermsActivity: $scenarios lifecycle and consent scenarios, $checks assertions PASS")
}
