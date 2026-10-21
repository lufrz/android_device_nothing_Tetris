/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.consent

import android.app.StatusBarManager
import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.os.SystemClock
import android.os.UserHandle
import android.view.WindowInsets
import android.widget.Toast
import androidx.activity.ComponentActivity
import androidx.activity.OnBackPressedCallback
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.setValue
import androidx.lifecycle.lifecycleScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.lineageos.settings.tetris.PartsActivity
import org.lineageos.settings.tetris.R
import org.lineageos.settings.tetris.ui.TetrisPartsTheme

/** A temporary decision window, never a persistent kiosk or a lock-screen replacement. */
class TermsActivity : ComponentActivity() {
    private var mandatory = false
    private var displayedEpoch = ""
    private var deadline = 0L
    private var remaining by mutableStateOf(20)
    private var busy by mutableStateOf(false)
    private var preparing by mutableStateOf(true)
    private var error by mutableStateOf(false)
    private var decided = false
    private var restrictionsApplied = false
    private val statusBar by lazy { getSystemService(StatusBarManager::class.java) }

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) { finish(); return }
        if (TermsConsent.synchronize(this)) { finish(); return }
        mandatory = intent.getBooleanExtra(EXTRA_MANDATORY, false)
        displayedEpoch = TermsConsent.epoch(this)
        deadline = TermsCountdown.deadline(SystemClock.elapsedRealtime(),
            savedInstanceState?.takeIf { it.getString(STATE_EPOCH) == displayedEpoch }
                ?.getLong(STATE_DEADLINE))
        remaining = TermsCountdown.remaining(deadline, SystemClock.elapsedRealtime())
        onBackPressedDispatcher.addCallback(this, object : OnBackPressedCallback(mandatory) {
            override fun handleOnBackPressed() = Unit
        })
        enableEdgeToEdge()
        setContent {
            TetrisPartsTheme {
                TermsScreen(mandatory, remaining, busy, error,
                    onAccept = { decide(true) }, onDecline = { decide(false) }, onBack = ::finish,
                    preparing = preparing)
            }
        }
        lifecycleScope.launch {
            val failures = withContext(Dispatchers.IO) { PartsFeatureReset.disable(applicationContext) }
            error = error || failures.isNotEmpty()
            preparing = false
        }
        lifecycleScope.launch {
            while (!decided) {
                remaining = TermsCountdown.remaining(deadline, SystemClock.elapsedRealtime())
                if (remaining == 0) break
                delay(200)
            }
        }
    }

    override fun onResume() {
        super.onResume()
        if (isFinishing || decided) return
        if (TermsConsent.synchronize(this)) { finish(); return }
        // Manual entry also counts as presentation: a pending unlock/retry must not
        // replace this navigable screen with the mandatory version later.
        if (!TermsConsent.markPresented(this)) error = true
        TermsLifecycle.cancelScheduledPrompt(this)
        if (mandatory) applyRestrictions()
    }

    private fun decide(accept: Boolean) {
        if (decided || busy || (accept && (preparing || error ||
                TermsCountdown.remaining(deadline, SystemClock.elapsedRealtime()) != 0))) return
        busy = true
        lifecycleScope.launch {
            val saved = withContext(Dispatchers.IO) {
                // Reset ran before presenting the actions; agreeing never re-enables old tweaks.
                TermsConsent.decide(applicationContext, displayedEpoch, accept)
            }
            if (!saved) {
                error = true
                busy = false
                // Refusal must never strand the owner behind a broken persistence operation.
                if (accept) return@launch
                Toast.makeText(this@TermsActivity, R.string.terms_save_error, Toast.LENGTH_LONG).show()
            }
            decided = true
            TermsLifecycle.cancelScheduledPrompt(this@TermsActivity)
            releaseRestrictions()
            if (accept && !mandatory) startActivity(Intent(this@TermsActivity, PartsActivity::class.java))
            finish()
        }
    }

    private fun applyRestrictions() {
        restrictionsApplied = true
        // Each manager owns a Binder token; Android removes its flags on process death.
        runCatching {
            statusBar?.disable(StatusBarManager.DISABLE_EXPAND or StatusBarManager.DISABLE_HOME or
                StatusBarManager.DISABLE_RECENT or StatusBarManager.DISABLE_BACK or StatusBarManager.DISABLE_SEARCH)
            statusBar?.disable2(StatusBarManager.DISABLE2_QUICK_SETTINGS or StatusBarManager.DISABLE2_NOTIFICATION_SHADE)
        }
        window.insetsController?.hide(WindowInsets.Type.navigationBars())
        window.insetsController?.show(WindowInsets.Type.statusBars())
    }

    private fun releaseRestrictions() {
        if (!restrictionsApplied) return
        restrictionsApplied = false
        runCatching { statusBar?.disable(StatusBarManager.DISABLE_NONE) }
        runCatching { statusBar?.disable2(StatusBarManager.DISABLE2_NONE) }
        window.insetsController?.show(WindowInsets.Type.navigationBars())
    }

    override fun onPause() { releaseRestrictions(); super.onPause() }
    override fun onStop() { releaseRestrictions(); super.onStop() }
    override fun onDestroy() { releaseRestrictions(); super.onDestroy() }
    override fun onSaveInstanceState(outState: Bundle) {
        outState.putString(STATE_EPOCH, displayedEpoch)
        outState.putLong(STATE_DEADLINE, deadline)
        super.onSaveInstanceState(outState)
    }

    companion object {
        private const val EXTRA_MANDATORY = "mandatory_decision"
        private const val STATE_DEADLINE = "accept_deadline"
        private const val STATE_EPOCH = "displayed_epoch"
        internal fun intent(context: Context, mandatory: Boolean) =
            Intent(context, TermsActivity::class.java).putExtra(EXTRA_MANDATORY, mandatory)
    }
}
