// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent

import android.app.ActivityManager
import android.app.KeyguardManager
import android.app.job.JobInfo
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.app.job.JobService
import android.content.BroadcastReceiver
import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.database.ContentObserver
import android.os.Handler
import android.os.Looper
import android.os.PowerManager
import android.os.SystemClock
import android.os.UserHandle
import android.os.UserManager
import android.provider.Settings
import android.util.Log

/** Boot republishes the gate; an update alone never schedules a terms window. */
class TermsBootReceiver : BroadcastReceiver() {
    override fun onReceive(context: Context, intent: Intent) {
        when (intent.action) {
            Intent.ACTION_LOCKED_BOOT_COMPLETED -> TermsLifecycle.refresh(context, allowUi = false)
            Intent.ACTION_BOOT_COMPLETED,
            Intent.ACTION_MY_PACKAGE_REPLACED -> TermsLifecycle.refresh(context, allowUi = true)
        }
    }
}

/** Settings-provider triggers survive the Parts process being killed during setup. */
class TermsSetupJobService : JobService() {
    override fun onStartJob(params: JobParameters): Boolean {
        TermsLifecycle.refresh(this, allowUi = true, fromJob = true)
        return false
    }

    override fun onStopJob(params: JobParameters): Boolean = true
}

internal object TermsLifecycle {
    private const val TAG = "TetrisPartsTerms"
    private const val JOB_NAMESPACE = "org.lineageos.settings.tetris.consent"
    private const val SETUP_JOB_ID = 1
    private const val FOREGROUND_JOB_ID = 2
    private const val RETRY_DELAY_MS = 30_000L
    private const val SETUP_SETTLE_MS = 500L

    // These callbacks all run on the main thread. A returned startActivity call is not an
    // acknowledgement: only TermsActivity.onResume records that the prompt was presented.
    private var launchRequestedAt: Long? = null
    private var pendingContext: Context? = null
    private var setupObserver: ContentObserver? = null
    private var lastStage: String? = null
    private val handler = Handler(Looper.getMainLooper())
    private val setupRefresh = Runnable { pendingContext?.let { refresh(it, allowUi = true) } }
    private val unlockReceiver = object : BroadcastReceiver() {
        override fun onReceive(context: Context, intent: Intent) {
            if (intent.action == Intent.ACTION_USER_PRESENT ||
                    intent.action == Intent.ACTION_USER_UNLOCKED) {
                refresh(context, allowUi = true)
            }
        }
    }

    fun refresh(context: Context, allowUi: Boolean, fromJob: Boolean = false) {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) return
        try {
            TermsConsent.synchronize(context)
            if (!TermsConsent.shouldPromptAutomatically(context)) {
                cancelScheduledPrompt(context)
                return
            }
            // Register unlock/setup observers only for changed terms or an explicit
            // prompt generation. Ordinary updates and fresh setups stop above.
            watchUnlock(context)
            if (!allowUi) return
            if (!setupComplete(context)) {
                stage("Waiting for setup completion")
                watchSetupCompletion(context, replaceExisting = fromJob)
                // The wizard may finish between the first read and observer registration.
                if (!setupComplete(context)) return
            }
            stopWatchingSetup()
            scheduler(context)?.cancel(SETUP_JOB_ID)
            if (!canShowPrompt(context)) {
                stage("Waiting for the unlocked owner")
                scheduleForegroundRetry(context, replaceExisting = fromJob)
                return
            }
            val now = SystemClock.elapsedRealtime()
            // A job armed while locked may fire just after an unlock launched the window.
            // Re-arm that consumed job even while duplicate launches are suppressed.
            scheduleForegroundRetry(context, replaceExisting = fromJob)
            if (launchRequestedAt?.let { now - it in 0 until RETRY_DELAY_MS } == true) return
            launchRequestedAt = now
            try {
                stage("Requesting automatic terms")
                context.startActivity(
                    TermsActivity.intent(context, mandatory = true)
                        .addFlags(Intent.FLAG_ACTIVITY_NEW_TASK)
                )
            } catch (error: RuntimeException) {
                launchRequestedAt = null
                Log.w(TAG, "Could not show terms", error)
            }
        } catch (error: RuntimeException) {
            Log.w(TAG, "Could not refresh terms state", error)
        }
    }

    fun cancelScheduledPrompt(context: Context) {
        launchRequestedAt = null
        stopWatchingSetup()
        pendingContext?.unregisterReceiver(unlockReceiver)
        pendingContext = null
        lastStage = null
        scheduler(context)?.let {
            it.cancel(SETUP_JOB_ID)
            it.cancel(FOREGROUND_JOB_ID)
        }
    }

    private fun watchUnlock(context: Context) {
        if (pendingContext != null) return
        // Manifest USER_PRESENT receivers are restricted on modern Android. A temporary
        // registration also handles USER_UNLOCKED, which is a registered-only broadcast.
        // SystemUI has its own UID in this ROM. Both actions are protected system
        // broadcasts, so receive them across UIDs without admitting app-defined actions.
        val app = context.applicationContext
        app.registerReceiver(unlockReceiver, IntentFilter().apply {
            addAction(Intent.ACTION_USER_PRESENT)
            addAction(Intent.ACTION_USER_UNLOCKED)
        }, Context.RECEIVER_EXPORTED)
        pendingContext = app
    }

    private fun stopWatchingSetup() {
        handler.removeCallbacks(setupRefresh)
        setupObserver?.let { pendingContext?.contentResolver?.unregisterContentObserver(it) }
        setupObserver = null
    }

    private fun stage(value: String) {
        if (lastStage == value) return
        lastStage = value
        Log.i(TAG, value)
    }

    private fun setupComplete(context: Context): Boolean =
        Settings.Global.getInt(
            context.contentResolver, Settings.Global.DEVICE_PROVISIONED, 0
        ) != 0 && Settings.Secure.getInt(
            context.contentResolver, Settings.Secure.USER_SETUP_COMPLETE, 0
        ) != 0

    private fun canShowPrompt(context: Context): Boolean =
        context.getSystemService(UserManager::class.java)?.isUserUnlocked == true &&
            ActivityManager.getCurrentUser() == UserHandle.USER_SYSTEM &&
            context.getSystemService(PowerManager::class.java)?.isInteractive == true &&
            context.getSystemService(KeyguardManager::class.java)?.isKeyguardLocked == false

    private fun scheduler(context: Context): JobScheduler? =
        context.getSystemService(JobScheduler::class.java)?.forNamespace(JOB_NAMESPACE)

    private fun watchSetupCompletion(context: Context, replaceExisting: Boolean) {
        if (setupObserver == null) {
            val observer = object : ContentObserver(handler) {
                override fun onChange(selfChange: Boolean) {
                    // SetupWizard still has to finish its Activity and launch home after
                    // writing the flags. Coalesce both writes before opening our window.
                    handler.removeCallbacks(setupRefresh)
                    handler.postDelayed(setupRefresh, SETUP_SETTLE_MS)
                }
            }
            setupObserver = observer
            val resolver = pendingContext!!.contentResolver
            resolver.registerContentObserver(
                Settings.Global.getUriFor(Settings.Global.DEVICE_PROVISIONED), false, observer)
            resolver.registerContentObserver(
                Settings.Secure.getUriFor(Settings.Secure.USER_SETUP_COMPLETE), false, observer)
        }
        // The job survives process death; the local observer avoids waiting for batching
        // when Parts is still running at the end of SetupWizard.
        val scheduler = scheduler(context) ?: return
        scheduler.cancel(FOREGROUND_JOB_ID)
        // A fired content job disappears when onStartJob returns false. Replace that job if
        // only one of the two setup flags has changed, but keep an existing unfired observer.
        if (!replaceExisting && scheduler.getPendingJob(SETUP_JOB_ID) != null) return
        val job = JobInfo.Builder(
            SETUP_JOB_ID, ComponentName(context, TermsSetupJobService::class.java)
        ).addTriggerContentUri(
            JobInfo.TriggerContentUri(
                Settings.Global.getUriFor(Settings.Global.DEVICE_PROVISIONED), 0
            )
        ).addTriggerContentUri(
            JobInfo.TriggerContentUri(
                Settings.Secure.getUriFor(Settings.Secure.USER_SETUP_COMPLETE), 0
            )
        ).setTriggerContentUpdateDelay(0)
            .setTriggerContentMaxDelay(0)
            .build()
        if (scheduler.schedule(job) != JobScheduler.RESULT_SUCCESS) {
            Log.w(TAG, "Could not watch setup completion")
        }
    }

    private fun scheduleForegroundRetry(context: Context, replaceExisting: Boolean) {
        val scheduler = scheduler(context) ?: return
        if (!replaceExisting && scheduler.getPendingJob(FOREGROUND_JOB_ID) != null) return
        val job = JobInfo.Builder(
            FOREGROUND_JOB_ID, ComponentName(context, TermsSetupJobService::class.java)
        ).setMinimumLatency(RETRY_DELAY_MS)
            .setOverrideDeadline(RETRY_DELAY_MS * 2)
            .build()
        if (scheduler.schedule(job) != JobScheduler.RESULT_SUCCESS) {
            Log.w(TAG, "Could not defer terms until the owner is active")
        }
    }
}
