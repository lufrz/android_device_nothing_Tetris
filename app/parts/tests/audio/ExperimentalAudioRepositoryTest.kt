/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris.audio

import android.media.AudioSystem
import android.os.SystemProperties
import java.io.IOException
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlin.concurrent.thread
import org.lineageos.settings.tetris.consent.PartsControlGate

private const val PROPERTY = "persist.sys.tetris.audio_mode"

private fun parameters(requested: String = "off", reason: String = "idle"): String =
    "tetris_audio_version=1;tetris_audio_available=1;tetris_audio_stereo_available=1;" +
        "tetris_audio_requested=$requested;tetris_audio_active=off;tetris_audio_reason=$reason"

fun main(args: Array<String>) {
    if (args.singleOrNull() == "restart") {
        // This entry runs in a fresh JVM: the saved preference predates first repository access.
        SystemProperties.values[PROPERTY] = "stereo"
        AudioSystem.readAction = { parameters("stereo") }
        val control = ExperimentalAudioRepository.control
        repeat(3) {
            val state = control.read()
            check(state.requested == ExperimentalAudioMode.STEREO)
            check(state.active == ExperimentalAudioMode.OFF)
        }
        check(SystemProperties.get(PROPERTY) == "stereo")
        check(SystemProperties.writes.isEmpty() && AudioSystem.writes.isEmpty())
        println("PASS: fresh repository reads a saved mode without overwriting or replaying it")
        return
    }
    var cases = 0
    fun test(name: String, block: () -> Unit) {
        AudioSystem.reset()
        SystemProperties.reset()
        AudioSystem.readAction = { parameters() }
        // Refresh the process singleton as a new resumed page does; it must not replay a request.
        ExperimentalAudioRepository.control.read()
        check(AudioSystem.writes.isEmpty() && SystemProperties.writes.isEmpty())
        block()
        check(AudioSystem.reads.all { it == "tetris_audio" })
        check(SystemProperties.writes.all { it.first == PROPERTY })
        cases++
        println("PASS: $name")
    }

    test("closed consent rejects queued mode persistence and HAL writes") {
        PartsControlGate.allowed = false
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.DUAL_MONO)
        check(!result.accepted)
        check(SystemProperties.writes.isEmpty() && AudioSystem.writes.isEmpty())
    }
    test("revocation is immediate and worker cleanup follows an old transaction rollback") {
        SystemProperties.values[PROPERTY] = "dual_mono"
        AudioSystem.readAction = { parameters("dual_mono") }
        val entered = CountDownLatch(1)
        val release = CountDownLatch(1)
        val drained = CountDownLatch(1)
        AudioSystem.writeAction = { request ->
            if (request == "tetris_audio_mode=stereo") {
                entered.countDown()
                check(release.await(5, TimeUnit.SECONDS))
                -1
            } else 0
        }
        val operation = thread { ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO) }
        check(entered.await(5, TimeUnit.SECONDS))
        // This must return while the old Binder transaction is still blocked.
        PartsControlGate.allowed = false
        check(operation.isAlive && release.count == 1L)
        val cleanup = thread {
            PartsControlGate.awaitPendingMutations()
            SystemProperties.set(PROPERTY, "off")
            drained.countDown()
        }
        release.countDown()
        check(drained.await(5, TimeUnit.SECONDS))
        operation.join(); cleanup.join()
        check(SystemProperties.get(PROPERTY) == "off")
        val writes = AudioSystem.writes.size
        check(!ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO).accepted)
        check(AudioSystem.writes.size == writes && SystemProperties.get(PROPERTY) == "off")
    }
    test("reading and repository initialization never authorize or send a mode") {
        repeat(3) { ExperimentalAudioRepository.control.read() }
        check(AudioSystem.writes.isEmpty() && SystemProperties.writes.isEmpty())
    }
    test("property permission rejection prevents the HAL request") {
        SystemProperties.values[PROPERTY] = "off"
        SystemProperties.setFailure = { _, _ -> SecurityException("Denied") }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.DUAL_MONO)
        check(!result.accepted && result.state.requested == ExperimentalAudioMode.OFF)
        check(AudioSystem.writes.isEmpty())
        check(SystemProperties.values[PROPERTY] == "off")
    }
    test("mode authorization is present before the matching HAL request") {
        AudioSystem.writeAction = { request ->
            check(request == "tetris_audio_mode=dual_mono")
            check(SystemProperties.get(PROPERTY) == "dual_mono")
            AudioSystem.readAction = { parameters("dual_mono") }
            0
        }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.DUAL_MONO)
        check(result.accepted && result.state.requested == ExperimentalAudioMode.DUAL_MONO)
        check(SystemProperties.writes == listOf(PROPERTY to "dual_mono"))
        check(AudioSystem.writes == listOf("tetris_audio_mode=dual_mono"))
    }
    test("unknown HAL accepting an OFF write does not retain new authorization") {
        SystemProperties.values[PROPERTY] = "dual_mono"
        AudioSystem.readAction = { "other_hal_reply=ok" }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.OFF)
        check(!result.accepted)
        check(AudioSystem.writes == listOf("tetris_audio_mode=off", "tetris_audio_mode=dual_mono"))
        check(SystemProperties.writes == listOf(PROPERTY to "off", PROPERTY to "dual_mono"))
        check(SystemProperties.get(PROPERTY) == "dual_mono")
    }
    test("unchanged readback rolls authorization back instead of accepting a no-op HAL") {
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO)
        check(!result.accepted && result.state.requested == ExperimentalAudioMode.OFF)
        check(SystemProperties.writes == listOf(PROPERTY to "stereo", PROPERTY to "off"))
        check(SystemProperties.get(PROPERTY) == "off")
    }
    test("missing versioned readback after HAL write rolls authorization back") {
        AudioSystem.writeAction = { AudioSystem.readAction = { null }; 0 }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.DUAL_MONO)
        check(!result.accepted && !result.state.available)
        check(SystemProperties.get(PROPERTY) == "off")
        check(SystemProperties.writes == listOf(PROPERTY to "dual_mono", PROPERTY to "off"))
    }
    test("accepted HAL request with lost readback restores live mode and saved choice") {
        var liveMode = "off"
        var dropNextRead = false
        AudioSystem.readAction = {
            if (dropNextRead) {
                dropNextRead = false
                null
            } else parameters(liveMode)
        }
        AudioSystem.writeAction = { request ->
            liveMode = request.substringAfter('=')
            check(SystemProperties.get(PROPERTY) == liveMode)
            if (liveMode == "stereo") dropNextRead = true
            0
        }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO)
        check(!result.accepted && result.state.requested == ExperimentalAudioMode.OFF)
        check(liveMode == "off" && SystemProperties.get(PROPERTY) == "off")
        check(AudioSystem.writes == listOf("tetris_audio_mode=stereo", "tetris_audio_mode=off"))
    }
    test("accepted HAL request with throwing readback restores prior non-OFF selection") {
        var liveMode = "dual_mono"
        var throwNextRead = false
        SystemProperties.values[PROPERTY] = liveMode
        AudioSystem.readAction = {
            if (throwNextRead) {
                throwNextRead = false
                throw IOException("Verification reply lost")
            }
            parameters(liveMode)
        }
        AudioSystem.writeAction = { request ->
            liveMode = request.substringAfter('=')
            check(SystemProperties.get(PROPERTY) == liveMode)
            if (liveMode == "stereo") throwNextRead = true
            0
        }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO)
        check(!result.accepted && result.state.requested == ExperimentalAudioMode.DUAL_MONO)
        check(liveMode == "dual_mono" && SystemProperties.get(PROPERTY) == "dual_mono")
        check(AudioSystem.writes == listOf("tetris_audio_mode=stereo", "tetris_audio_mode=dual_mono"))
    }
    test("HAL failure rolls back even if a concurrent state already matches") {
        AudioSystem.writeAction = {
            AudioSystem.readAction = { parameters("stereo") }
            -1
        }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO)
        check(!result.accepted && result.state.requested == ExperimentalAudioMode.STEREO)
        check(SystemProperties.get(PROPERTY) == "off")
    }
    test("throwing HAL write rolls authorization back and reports refusal") {
        AudioSystem.writeAction = { throw IOException("Audio server died") }
        check(!ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO).accepted)
        check(SystemProperties.get(PROPERTY) == "off")
        check(SystemProperties.writes == listOf(PROPERTY to "stereo", PROPERTY to "off"))
    }
    test("throwing readback rolls authorization back and reports refusal") {
        AudioSystem.writeAction = {
            AudioSystem.readAction = { throw IOException("Audio server died during verification") }
            0
        }
        check(!ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.DUAL_MONO).accepted)
        check(SystemProperties.get(PROPERTY) == "off")
    }
    test("rollback does not overwrite a concurrently replaced authorization") {
        AudioSystem.writeAction = {
            SystemProperties.values[PROPERTY] = "off"
            -1
        }
        check(!ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO).accepted)
        check(SystemProperties.writes == listOf(PROPERTY to "stereo"))
        check(AudioSystem.writes == listOf("tetris_audio_mode=stereo"))
        check(SystemProperties.get(PROPERTY) == "off")
    }
    test("newer authorization during runtime rollback is preserved") {
        AudioSystem.writeAction = { request ->
            if (request == "tetris_audio_mode=off") {
                SystemProperties.values[PROPERTY] = "dual_mono"
                AudioSystem.readAction = { parameters("dual_mono") }
            }
            -1
        }
        val result = ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO)
        check(!result.accepted && result.state.requested == ExperimentalAudioMode.DUAL_MONO)
        check(SystemProperties.get(PROPERTY) == "dual_mono")
        check(SystemProperties.writes == listOf(PROPERTY to "stereo", PROPERTY to "off"))
        check(AudioSystem.writes == listOf("tetris_audio_mode=stereo", "tetris_audio_mode=off"))
    }
    test("rollback restores the prior valid selection rather than always OFF") {
        SystemProperties.values[PROPERTY] = "dual_mono"
        AudioSystem.readAction = { parameters("dual_mono") }
        check(!ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO).accepted)
        check(SystemProperties.get(PROPERTY) == "dual_mono")
        check(SystemProperties.writes == listOf(PROPERTY to "stereo", PROPERTY to "dual_mono"))
    }
    test("rollback rejection is contained and does not claim success") {
        SystemProperties.setFailure = { _, value ->
            if (value == "off") SecurityException("Permission revoked") else null
        }
        check(!ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.STEREO).accepted)
        check(SystemProperties.writes == listOf(PROPERTY to "stereo", PROPERTY to "off"))
    }
    test("successful disabling remains explicitly authorized and acknowledged") {
        SystemProperties.values[PROPERTY] = "stereo"
        AudioSystem.readAction = { parameters("stereo") }
        AudioSystem.writeAction = { request ->
            check(request == "tetris_audio_mode=off" && SystemProperties.get(PROPERTY) == "off")
            AudioSystem.readAction = { parameters("off") }
            0
        }
        check(ExperimentalAudioRepository.control.apply(ExperimentalAudioMode.OFF).accepted)
        check(SystemProperties.writes == listOf(PROPERTY to "off"))
    }
    println("$cases experimental audio repository tests passed")
}
