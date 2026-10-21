// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent

import android.content.Context
import java.util.concurrent.CountDownLatch

object TermsConsent {
    @Volatile var accepted = false
    @Volatile var currentEpoch = "terms-one"
    @Volatile var saveSucceeds = true
    @Volatile var markSucceeds = true
    @Volatile var markCount = 0
    @Volatile var decisionCount = 0
    @Volatile var lastDecision: Pair<String, Boolean>? = null
    @Volatile var decisionGate: CountDownLatch? = null
    fun synchronize(context: Context) = accepted
    fun epoch(context: Context) = currentEpoch
    fun markPresented(context: Context): Boolean { markCount++; return markSucceeds }
    fun decide(context: Context, epoch: String, accept: Boolean): Boolean {
        decisionCount++
        lastDecision = epoch to accept
        decisionGate?.await()
        if (epoch != currentEpoch || !saveSucceeds) return false
        accepted = accept
        return true
    }
    fun reset() {
        accepted = false; currentEpoch = "terms-one"; saveSucceeds = true
        markSucceeds = true; markCount = 0; decisionCount = 0; lastDecision = null
        decisionGate = null
    }
}
object PartsFeatureReset {
    @Volatile var failures = emptyList<String>()
    @Volatile var calls = 0
    @Volatile var gate: CountDownLatch? = null
    fun disable(context: Context): List<String> {
        calls++
        gate?.await()
        return failures
    }
    fun reset() { failures = emptyList(); calls = 0; gate = null }
}
object TermsLifecycle {
    @Volatile var cancelCount = 0
    fun cancelScheduledPrompt(context: Context) { cancelCount++ }
}
data class ScreenState(
    val mandatory: Boolean,
    val remaining: Int,
    val busy: Boolean,
    val error: Boolean,
    val preparing: Boolean,
    val accept: () -> Unit,
    val decline: () -> Unit,
    val back: () -> Unit,
)
object Screen { var current: ScreenState? = null }
fun TermsScreen(mandatory: Boolean, remainingSeconds: Int, busy: Boolean, error: Boolean,
    onAccept: () -> Unit, onDecline: () -> Unit, onBack: () -> Unit, preparing: Boolean = false) {
    Screen.current = ScreenState(mandatory, remainingSeconds, busy, error, preparing,
        onAccept, onDecline, onBack)
}
