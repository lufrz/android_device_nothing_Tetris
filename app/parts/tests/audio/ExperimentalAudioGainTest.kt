// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.audio

import android.media.AudioSystem
import android.os.SystemProperties

private const val GAIN_PROPERTY = "persist.sys.tetris.audio_gain"
private fun gainReply(gain: Int, supported: Boolean = true) =
    "tetris_audio_version=1;tetris_audio_available=1;tetris_audio_stereo_available=1;" +
        "tetris_audio_requested=stereo;tetris_audio_active=stereo;tetris_audio_reason=playing;" +
        "tetris_audio_gain_available=${if (supported) 1 else 0};tetris_audio_gain_db=$gain"

fun main() {
    val control = ExperimentalAudioRepository.control
    AudioSystem.reset()
    SystemProperties.reset()
    var liveGain = 0
    AudioSystem.readAction = { gainReply(liveGain) }
    AudioSystem.writeAction = { request ->
        check(request.startsWith("tetris_audio_gain="))
        liveGain = request.substringAfter('=').toInt()
        check(SystemProperties.get(GAIN_PROPERTY) == liveGain.toString())
        0
    }
    for (gain in -10..6) {
        val result = control.applyGain(gain)
        check(result.accepted && result.state.gainDb == gain)
        check(result.state.active == ExperimentalAudioMode.STEREO)
        check(SystemProperties.get(GAIN_PROPERTY) == gain.toString())
    }
    check(SystemProperties.writes.all { it.first == GAIN_PROPERTY })
    println("PASS: all gain levels persist independently of the audio mode")

    val sent = AudioSystem.writes.size
    check(!control.applyGain(-11).accepted && !control.applyGain(7).accepted &&
        !control.applyGain(8).accepted &&
        !control.applyGain(9).accepted &&
        !control.applyGain(15).accepted)
    AudioSystem.readAction = { gainReply(0, false) }
    check(!control.applyGain(3).accepted)
    check(AudioSystem.writes.size == sent)
    val oldReply = gainReply(0).substringBefore(";tetris_audio_gain_available")
    check(ExperimentalAudioState.parse(oldReply)?.gainAvailable == false)
    listOf("7", "8", "9", "15", "-11", "bogus", "2147483648").forEach {
        check(ExperimentalAudioState.parse(gainReply(0).replace("gain_db=0", "gain_db=$it")) == null)
    }
    println("PASS: unsupported kernels and invalid gains do not write")

    liveGain = 2
    SystemProperties.values[GAIN_PROPERTY] = "2"
    AudioSystem.readAction = { gainReply(liveGain) }
    AudioSystem.writeAction = { request ->
        liveGain = request.substringAfter('=').toInt()
        if (liveGain == 4) -1 else 0
    }
    check(!control.applyGain(4).accepted)
    check(liveGain == 2 && SystemProperties.get(GAIN_PROPERTY) == "2")
    println("PASS: rejected gain restores both saved and live values")

    var dropRead = false
    AudioSystem.readAction = { if (dropRead) { dropRead = false; null } else gainReply(liveGain) }
    AudioSystem.writeAction = { request ->
        liveGain = request.substringAfter('=').toInt()
        if (liveGain == 5) dropRead = true
        0
    }
    check(!control.applyGain(5).accepted)
    check(liveGain == 2 && SystemProperties.get(GAIN_PROPERTY) == "2")
    println("PASS: lost acknowledgement restores the prior gain")

    for (previous in listOf("7", "8")) {
        liveGain = 6
        SystemProperties.values[GAIN_PROPERTY] = previous
        check(!control.applyGain(5).accepted)
        check(liveGain == 6 && SystemProperties.get(GAIN_PROPERTY) == "6")
    }
    println("PASS: rollback of old higher preferences restores the supported ceiling")
    liveGain = 2
    SystemProperties.values[GAIN_PROPERTY] = "2"

    val beforeRead = AudioSystem.writes.size
    repeat(3) { check(control.read().gainDb == 2) }
    check(AudioSystem.writes.size == beforeRead)
    SystemProperties.setFailure = { _, _ -> SecurityException("Denied") }
    check(!control.applyGain(3).accepted)
    check(AudioSystem.writes.size == beforeRead)
    println("PASS: reads and failed authorization never change gain")
}
