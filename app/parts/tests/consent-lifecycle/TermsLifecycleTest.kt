// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent

import android.app.ActivityManager
import android.app.KeyguardManager
import android.app.job.JobParameters
import android.app.job.JobScheduler
import android.content.Context
import android.content.Intent
import android.content.IntentFilter
import android.content.BroadcastReceiver
import android.os.Handler
import android.os.PowerManager
import android.os.SystemClock
import android.os.UserHandle
import android.os.UserManager
import android.provider.Settings

private var cases = 0
private val provisionedUri = Settings.Global.getUriFor(Settings.Global.DEVICE_PROVISIONED)
private val setupUri = Settings.Secure.getUriFor(Settings.Secure.USER_SETUP_COMPLETE)

private fun scenario(name: String, test: (Context, JobScheduler) -> Unit) {
    val context = Context()
    val jobs = JobScheduler()
    context.services[JobScheduler::class.java] = jobs
    context.services[UserManager::class.java] = UserManager()
    context.services[PowerManager::class.java] = PowerManager()
    context.services[KeyguardManager::class.java] = KeyguardManager()
    context.contentResolver.values[Settings.Global.DEVICE_PROVISIONED] = 1
    context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 1
    ActivityManager.user = 0
    UserHandle.user = 0
    TermsConsent.accepted = false
    TermsConsent.shown = false
    TermsConsent.synchronizations = 0
    TermsLifecycle.cancelScheduledPrompt(context)
    Handler.clear()
    SystemClock.now = 100_000L
    test(context, jobs)
    cases++
    println("PASS $name")
}

private fun noListeners(context: Context) {
    check(context.receivers.isEmpty()) { "An unnecessary unlock receiver remains" }
    check(context.contentResolver.observers.isEmpty()) { "An unnecessary setup observer remains" }
}

private fun oneUnlockReceiver(context: Context) {
    val registration = context.receivers.single()
    check(registration.context === context.applicationContext)
    check(registration.flags == Context.RECEIVER_EXPORTED)
    check(registration.filter.actions == setOf(Intent.ACTION_USER_PRESENT, Intent.ACTION_USER_UNLOCKED))
}

private fun foregroundJob(jobs: JobScheduler) = jobs.jobs.values.single { it.minimumLatency > 0 }

private fun fireForegroundJob(context: Context, jobs: JobScheduler) {
    val pending = foregroundJob(jobs)
    check(SystemClock.now >= jobs.deadlines.getValue(pending.id))
    jobs.cancel(pending.id)
    val service = TermsSetupJobService()
    service.application = context
    check(!service.onStartJob(JobParameters()))
}

fun main() {
    val receiver = TermsBootReceiver()
    scenario("accepted ordinary boot publishes gate without work or observers") { context, jobs ->
        TermsConsent.accepted = true
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        check(TermsConsent.synchronizations == 1 && context.starts.isEmpty() && jobs.jobs.isEmpty())
        noListeners(context)
    }
    scenario("declined same-epoch boot never reopens or watches unlock") { context, jobs ->
        TermsConsent.shown = true
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT), Context.SYSTEM_UI_UID)
        check(context.starts.isEmpty() && jobs.jobs.isEmpty())
        noListeners(context)
    }
    for (accepted in listOf(true, false)) {
        scenario("same-terms APK replacement preserves ${if (accepted) "acceptance" else "refusal"} without UI or watchers") { context, jobs ->
            TermsConsent.accepted = accepted
            TermsConsent.shown = true
            receiver.onReceive(context, Intent(Intent.ACTION_MY_PACKAGE_REPLACED))
            context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT), Context.SYSTEM_UI_UID)
            check(TermsConsent.synchronizations == 1 && context.starts.isEmpty() && jobs.jobs.isEmpty())
            noListeners(context)
        }
    }
    scenario("fresh baseline never arms automatic presentation during setup or unlock") { context, jobs ->
        // New installations establish a presented baseline without accepting the terms.
        TermsConsent.shown = true
        context.contentResolver.values[Settings.Global.DEVICE_PROVISIONED] = 0
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        context.getSystemService(UserManager::class.java)!!.isUserUnlocked = false
        receiver.onReceive(context, Intent(Intent.ACTION_LOCKED_BOOT_COMPLETED))
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        receiver.onReceive(context, Intent(Intent.ACTION_MY_PACKAGE_REPLACED))
        noListeners(context)
        check(jobs.jobs.isEmpty() && jobs.schedules == 0)
        context.contentResolver.setAndNotify(Settings.Global.DEVICE_PROVISIONED, 1, provisionedUri)
        context.contentResolver.setAndNotify(Settings.Secure.USER_SETUP_COMPLETE, 1, setupUri)
        context.getSystemService(UserManager::class.java)!!.isUserUnlocked = true
        context.sendBroadcast(Intent(Intent.ACTION_USER_UNLOCKED))
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT), Context.SYSTEM_UI_UID)
        SystemClock.advanceBy(60_000)
        check(!TermsConsent.accepted && context.starts.isEmpty() && jobs.jobs.isEmpty())
        check(Handler.pendingCount() == 0)
        noListeners(context)
    }
    scenario("locked boot publishes gate and registers only the pending unlock receiver") { context, jobs ->
        context.getSystemService(UserManager::class.java)!!.isUserUnlocked = false
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        receiver.onReceive(context, Intent(Intent.ACTION_LOCKED_BOOT_COMPLETED))
        check(TermsConsent.synchronizations == 1 && context.starts.isEmpty() && jobs.jobs.isEmpty())
        check(context.contentResolver.observers.isEmpty())
        oneUnlockReceiver(context)
    }
    scenario("accepted locked boot does not register unlock callbacks") { context, jobs ->
        TermsConsent.accepted = true
        receiver.onReceive(context, Intent(Intent.ACTION_LOCKED_BOOT_COMPLETED))
        check(jobs.jobs.isEmpty())
        noListeners(context)
    }
    scenario("changed terms revision launches once and retains fallback until Activity resume") { context, jobs ->
        receiver.onReceive(context, Intent(Intent.ACTION_MY_PACKAGE_REPLACED))
        val deadline = jobs.deadlines.getValue(foregroundJob(jobs).id)
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        check(context.starts.size == 1)
        check(context.starts.single().flags and Intent.FLAG_ACTIVITY_NEW_TASK != 0)
        check(jobs.deadlines.getValue(foregroundJob(jobs).id) == deadline)
        check(!TermsConsent.shown)
        oneUnlockReceiver(context)
    }
    scenario("secondary profile cannot synchronize, observe or present owner controls") { context, jobs ->
        UserHandle.user = 10
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        check(TermsConsent.synchronizations == 0 && context.starts.isEmpty() && jobs.jobs.isEmpty())
        noListeners(context)
    }
    scenario("unrecognized manifest receiver action is ignored") { context, _ ->
        receiver.onReceive(context, Intent("untrusted_setup_complete"))
        check(TermsConsent.synchronizations == 0 && context.starts.isEmpty())
        noListeners(context)
    }
    scenario("setup watches both settings directly and through a namespaced fallback") { context, jobs ->
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        check(context.starts.isEmpty())
        check(jobs.jobs.values.single().uris.toSet() == setOf(provisionedUri, setupUri))
        check(context.contentResolver.observers.map { it.first }.toSet() == setOf(provisionedUri, setupUri))
        check(jobs.namespace == "org.lineageos.settings.tetris.consent")
        TermsLifecycle.refresh(context, true)
        check(jobs.schedules == 1 && context.contentResolver.observers.size == 2)
        oneUnlockReceiver(context)
    }
    scenario("fired setup job rearms when only one setup flag is complete") { context, jobs ->
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        TermsLifecycle.refresh(context, true)
        TermsLifecycle.refresh(context, true, fromJob = true)
        check(jobs.schedules == 2 && context.starts.isEmpty())
        check(context.contentResolver.observers.size == 2)
    }
    scenario("setup completion during observer registration is not lost") { context, jobs ->
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        context.contentResolver.onRegister = {
            context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 1
        }
        TermsLifecycle.refresh(context, true)
        check(context.starts.size == 1)
        check(context.contentResolver.observers.isEmpty())
        check(jobs.jobs.values.none { it.uris.isNotEmpty() })
        foregroundJob(jobs)
    }
    scenario("setup completion while the fallback job registers is not lost") { context, jobs ->
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        jobs.onSchedule = {
            context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 1
        }
        TermsLifecycle.refresh(context, true)
        check(context.starts.size == 1 && context.contentResolver.observers.isEmpty())
        check(jobs.jobs.values.none { it.uris.isNotEmpty() })
    }
    scenario("owner in background waits without stealing a secondary screen") { context, jobs ->
        ActivityManager.user = 10
        TermsLifecycle.refresh(context, true)
        check(context.starts.isEmpty() && foregroundJob(jobs).minimumLatency == 30_000L)
        ActivityManager.user = 0
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        check(context.starts.size == 1)
    }
    scenario("locked user, keyguard and sleeping screen each prevent presentation") { context, jobs ->
        context.getSystemService(UserManager::class.java)!!.isUserUnlocked = false
        TermsLifecycle.refresh(context, true)
        context.getSystemService(UserManager::class.java)!!.isUserUnlocked = true
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = true
        TermsLifecycle.refresh(context, true)
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = false
        context.getSystemService(PowerManager::class.java)!!.isInteractive = false
        TermsLifecycle.refresh(context, true)
        check(context.starts.isEmpty() && jobs.jobs.size == 1 && jobs.schedules == 1)
        oneUnlockReceiver(context)
    }
    scenario("failed start retries through its actual job callback without marking terms shown") { context, jobs ->
        context.failStart = true
        TermsLifecycle.refresh(context, true)
        check(context.starts.isEmpty() && jobs.jobs.size == 1 && !TermsConsent.shown)
        context.failStart = false
        SystemClock.advanceBy(30_000)
        fireForegroundJob(context, jobs)
        check(context.starts.size == 1 && !TermsConsent.shown)
        foregroundJob(jobs)
    }
    scenario("job entry finishes and stop requests reschedule") { context, _ ->
        TermsConsent.shown = true
        val job = TermsSetupJobService()
        job.application = context
        check(!job.onStartJob(JobParameters()))
        check(job.onStopJob(JobParameters()))
    }
    scenario("real USER_PRESENT callback opens pending terms immediately after boot unlock") { context, jobs ->
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = true
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        check(context.starts.isEmpty())
        val schedules = jobs.schedules
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = false
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        check(context.starts.size == 1 && jobs.schedules == schedules)
        check(SystemClock.now == 100_000L)
    }
    scenario("USER_UNLOCKED alone cannot bypass keyguard and USER_PRESENT completes the handoff") { context, _ ->
        context.getSystemService(UserManager::class.java)!!.isUserUnlocked = false
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = true
        receiver.onReceive(context, Intent(Intent.ACTION_LOCKED_BOOT_COMPLETED))
        context.getSystemService(UserManager::class.java)!!.isUserUnlocked = true
        context.sendBroadcast(Intent(Intent.ACTION_USER_UNLOCKED))
        check(context.starts.isEmpty())
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = false
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        check(context.starts.size == 1)
    }
    scenario("spurious dynamic receiver actions do not cause a refresh") { context, _ ->
        receiver.onReceive(context, Intent(Intent.ACTION_LOCKED_BOOT_COMPLETED))
        val count = TermsConsent.synchronizations
        context.sendBroadcast(Intent("not_user_present"))
        val registration = context.receivers.single()
        registration.receiver.onReceive(context, Intent("not_user_present"))
        check(TermsConsent.synchronizations == count && context.starts.isEmpty())
    }
    scenario("delivery model rejects unexported receivers for the separate SystemUI UID") { context, _ ->
        var received = 0
        val observer = object : BroadcastReceiver() {
            override fun onReceive(context: Context, intent: Intent) { received++ }
        }
        context.registerReceiver(observer, IntentFilter(Intent.ACTION_USER_PRESENT), Context.RECEIVER_NOT_EXPORTED)
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT), Context.SYSTEM_UI_UID)
        check(received == 0)
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT), Context.SYSTEM_UID)
        check(received == 1)
        context.unregisterReceiver(observer)
    }
    scenario("exported pending receiver accepts only protected unlock actions and rejects spoofing") { context, _ ->
        receiver.onReceive(context, Intent(Intent.ACTION_LOCKED_BOOT_COMPLETED))
        oneUnlockReceiver(context)
        val synchronizations = TermsConsent.synchronizations
        for (action in listOf(Intent.ACTION_USER_PRESENT, Intent.ACTION_USER_UNLOCKED)) {
            val error = runCatching {
                context.sendBroadcast(Intent(action), Context.THIRD_PARTY_UID)
            }.exceptionOrNull()
            check(error is SecurityException)
        }
        context.sendBroadcast(Intent("untrusted_setup_complete"), Context.THIRD_PARTY_UID)
        context.receivers.single().receiver.onReceive(context, Intent("untrusted_setup_complete"))
        check(TermsConsent.synchronizations == synchronizations && context.starts.isEmpty())
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT), Context.SYSTEM_UI_UID)
        check(context.starts.size == 1)
    }
    scenario("actual setup callbacks debounce the two settings and launch after both complete") { context, jobs ->
        context.contentResolver.values[Settings.Global.DEVICE_PROVISIONED] = 0
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        context.contentResolver.setAndNotify(Settings.Global.DEVICE_PROVISIONED, 1, provisionedUri)
        SystemClock.advanceBy(200)
        context.contentResolver.setAndNotify(Settings.Secure.USER_SETUP_COMPLETE, 1, setupUri)
        SystemClock.advanceBy(499)
        check(context.starts.isEmpty())
        SystemClock.advanceBy(1)
        check(context.starts.size == 1)
        check(context.contentResolver.observers.isEmpty() && jobs.jobs.values.none { it.uris.isNotEmpty() })
        foregroundJob(jobs)
    }
    scenario("a callback for just one setup flag cannot show terms prematurely") { context, _ ->
        context.contentResolver.values[Settings.Global.DEVICE_PROVISIONED] = 0
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        context.contentResolver.setAndNotify(Settings.Global.DEVICE_PROVISIONED, 1, provisionedUri)
        SystemClock.advanceBy(500)
        check(context.starts.isEmpty() && context.contentResolver.observers.size == 2)
        context.contentResolver.setAndNotify(Settings.Secure.USER_SETUP_COMPLETE, 1, setupUri)
        SystemClock.advanceBy(500)
        check(context.starts.size == 1)
    }
    scenario("registration uses application context even when initiated from a transient service") { context, _ ->
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        val service = TermsSetupJobService()
        service.application = context
        check(!service.onStartJob(JobParameters()))
        oneUnlockReceiver(context)
        check(context.contentResolver.observers.size == 2)
        context.contentResolver.setAndNotify(Settings.Secure.USER_SETUP_COMPLETE, 1, setupUri)
        SystemClock.advanceBy(500)
        check(context.starts.size == 1)
    }
    scenario("accepted after registration cancels jobs, observers and queued setup callbacks") { context, jobs ->
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        TermsLifecycle.refresh(context, true)
        context.contentResolver.notifyChange(setupUri)
        check(Handler.pendingCount() > 0)
        TermsConsent.accepted = true
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        noListeners(context)
        check(jobs.jobs.isEmpty() && Handler.pendingCount() == 0)
        SystemClock.advanceBy(1000)
        check(context.starts.isEmpty())
    }
    scenario("new presentation policy cancels an old setup job and queued callbacks") { context, jobs ->
        context.contentResolver.values[Settings.Secure.USER_SETUP_COMPLETE] = 0
        TermsLifecycle.refresh(context, true)
        context.contentResolver.notifyChange(setupUri)
        check(jobs.jobs.isNotEmpty() && Handler.pendingCount() > 0)
        TermsConsent.shown = true
        receiver.onReceive(context, Intent(Intent.ACTION_MY_PACKAGE_REPLACED))
        noListeners(context)
        check(jobs.jobs.isEmpty() && Handler.pendingCount() == 0)
        context.contentResolver.setAndNotify(Settings.Secure.USER_SETUP_COMPLETE, 1, setupUri)
        SystemClock.advanceBy(60_000)
        check(context.starts.isEmpty())
    }
    scenario("an old foreground retry becomes inert when presentation is no longer pending") { context, jobs ->
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = true
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        foregroundJob(jobs)
        TermsConsent.shown = true
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = false
        SystemClock.advanceBy(30_000)
        fireForegroundJob(context, jobs)
        check(context.starts.isEmpty() && jobs.jobs.isEmpty())
        noListeners(context)
    }
    scenario("Activity resume cancels outstanding fallback and all listeners") { context, jobs ->
        TermsLifecycle.refresh(context, true)
        check(context.starts.size == 1 && jobs.jobs.isNotEmpty())
        TermsConsent.shown = true
        TermsLifecycle.cancelScheduledPrompt(context)
        noListeners(context)
        check(jobs.jobs.isEmpty())
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        check(context.starts.size == 1 && jobs.jobs.isEmpty())
        noListeners(context)
    }
    scenario("repeated unlock events before resume dedupe without postponing fallback") { context, jobs ->
        TermsLifecycle.refresh(context, true)
        val originalDeadline = jobs.deadlines.getValue(foregroundJob(jobs).id)
        val originalSchedules = jobs.schedules
        repeat(4) {
            SystemClock.advanceBy(5000)
            context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
            context.sendBroadcast(Intent(Intent.ACTION_USER_UNLOCKED))
        }
        check(context.starts.size == 1 && jobs.schedules == originalSchedules)
        check(jobs.deadlines.getValue(foregroundJob(jobs).id) == originalDeadline)
    }
    scenario("manually presented terms cannot be replaced by an automatic mandatory window") { context, jobs ->
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = true
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        check(context.starts.isEmpty() && jobs.jobs.isNotEmpty())
        // Manual TermsActivity.onResume records presentation and cancels pending lifecycle work.
        TermsConsent.shown = true
        TermsLifecycle.cancelScheduledPrompt(context)
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = false
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        SystemClock.advanceBy(60_000)
        TermsLifecycle.refresh(context, true, fromJob = true)
        check(context.starts.isEmpty() && jobs.jobs.isEmpty())
        noListeners(context)
    }
    scenario("silently dropped start survives and retries when its real fallback fires") { context, jobs ->
        context.dropStart = true
        TermsLifecycle.refresh(context, true)
        check(context.starts.isEmpty() && context.attempts.size == 1 && !TermsConsent.shown)
        foregroundJob(jobs)
        SystemClock.advanceBy(29_999)
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        check(context.attempts.size == 1)
        context.dropStart = false
        SystemClock.advanceBy(1)
        fireForegroundJob(context, jobs)
        check(context.attempts.size == 2 && context.starts.size == 1 && !TermsConsent.shown)
        foregroundJob(jobs)
    }
    scenario("expired pending start can retry from unlock even if JobScheduler has not run") { context, jobs ->
        context.dropStart = true
        TermsLifecycle.refresh(context, true)
        SystemClock.advanceBy(30_000)
        context.dropStart = false
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        check(context.attempts.size == 2 && context.starts.size == 1)
        foregroundJob(jobs)
    }
    scenario("consumed fallback rearms even when a recent unlock start is still pending") { context, jobs ->
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = true
        receiver.onReceive(context, Intent(Intent.ACTION_BOOT_COMPLETED))
        SystemClock.advanceBy(28_000)
        context.getSystemService(KeyguardManager::class.java)!!.isKeyguardLocked = false
        context.dropStart = true
        context.sendBroadcast(Intent(Intent.ACTION_USER_PRESENT))
        check(context.attempts.size == 1)
        SystemClock.advanceBy(2000)
        fireForegroundJob(context, jobs)
        check(context.attempts.size == 1)
        check(jobs.deadlines.getValue(foregroundJob(jobs).id) == SystemClock.now + 30_000)
        context.dropStart = false
        SystemClock.advanceBy(30_000)
        fireForegroundJob(context, jobs)
        check(context.attempts.size == 2 && context.starts.size == 1)
    }
    scenario("no setup observer is installed when both setup flags were already complete") { context, _ ->
        TermsLifecycle.refresh(context, true)
        check(context.contentResolver.observers.isEmpty())
    }
    println("Terms lifecycle: $cases scenarios passed")
}
