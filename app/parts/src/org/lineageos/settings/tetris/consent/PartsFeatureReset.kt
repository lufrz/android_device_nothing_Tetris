/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.consent

import android.content.Context
import android.content.Intent
import android.media.AudioSystem
import android.os.SystemClock
import android.os.SystemProperties
import org.lineageos.settings.tetris.adaptive.AdaptiveRepository
import org.lineageos.settings.tetris.adaptive.AdaptiveService
import org.lineageos.settings.tetris.audio.ExperimentalAudioMode
import org.lineageos.settings.tetris.audio.ExperimentalAudioState
import org.lineageos.settings.tetris.cpu.CpuControlSession
import org.lineageos.settings.tetris.gpu.GpuControlSession
import org.lineageos.settings.tetris.network.TcpControlSession
import org.lineageos.settings.tetris.thermal.ThermalController

/** Worker-thread cleanup. Restore only Parts-owned changes; never disable ordinary phone services. */
internal object PartsFeatureReset {
    @Synchronized fun disable(context: Context): List<String> {
        PartsControlGate.allowed = false
        val app = context.applicationContext
        val failed = mutableListOf<String>()
        fun reset(name: String, action: () -> Boolean) {
            if (!runCatching(action).getOrDefault(false)) failed += name
        }
        reset("consent") {
            val cleared = TcpControlSession.clearPendingRequests()
            SystemProperties.set("sys.tetrisparts.consent", "0")
            SystemProperties.get("sys.tetrisparts.consent") == "0" && cleared
        }
        // Revocation itself is immediate. Wait only on this worker for any old
        // property/Binder transaction (including its rollback) before resetting.
        PartsControlGate.awaitPendingMutations()
        reset("adaptive") {
            val restored = AdaptiveRepository.get(app).disableForConsent()
            app.stopService(Intent(app, AdaptiveService::class.java))
            AdaptiveService.clearProposalNotification(app)
            restored
        }
        reset("cpu") { CpuControlSession.releasePartsOverrides().successful }
        reset("gpu") { GpuControlSession.releasePartsOverrides().successful }
        reset("tcp") { TcpControlSession.manager.disableForConsent() }
        reset("audio") { disableAudio() }
        reset("thermal") { restoreThermal() }
        return failed
    }

    private fun disableAudio(): Boolean {
        val defaults = mapOf(
            "mode" to "off", "gain" to "0", "software_gain" to "0",
            "sync_trim" to "0", "output" to "both",
        )
        defaults.forEach { (name, value) ->
            SystemProperties.set("persist.sys.tetris.audio_$name", value)
        }
        // The off command revokes the live kernel lease. Saving properties alone
        // would leave an already-running stream active until its next callback.
        AudioSystem.setParameters("tetris_audio_mode=off")
        val status = AudioSystem.getParameters("tetris_audio")
        val observed = ExperimentalAudioState.parse(status)
        val stopped = status.isNullOrBlank() || observed != null &&
            observed.active == ExperimentalAudioMode.OFF && observed.requested == ExperimentalAudioMode.OFF
        return stopped && defaults.all { (name, value) ->
            SystemProperties.get("persist.sys.tetris.audio_$name") == value
        }
    }

    private fun restoreThermal(): Boolean {
        val before = ThermalController.read()
        val requested = SystemProperties.get("sys.tetrisparts.thermal_disabled", "0") == "1"
        if (!requested && before.mode != "disabled" && before.mode != "changing") return true
        ThermalController.request(false)
        val deadline = SystemClock.elapsedRealtime() + 10_000
        do {
            val current = ThermalController.read()
            if (current.resultId != before.resultId && current.mode == "enabled") return true
            if (current.resultId != before.resultId && current.mode != "changing") return false
            Thread.sleep(100)
        } while (SystemClock.elapsedRealtime() < deadline)
        return false
    }
}
