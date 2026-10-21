// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.audio

import androidx.compose.runtime.Composable
import androidx.compose.runtime.LaunchedEffect
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.rememberCoroutineScope
import androidx.compose.runtime.setValue
import androidx.compose.ui.res.stringResource
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.delay
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.lineageos.settings.tetris.R
import org.lineageos.settings.tetris.ui.rememberPartsResumed

@Composable
fun ExperimentalAudioPage(onBack: () -> Unit) {
    val resumed = rememberPartsResumed()
    val scope = rememberCoroutineScope()
    var state by remember { mutableStateOf(ExperimentalAudioState()) }
    var busy by remember { mutableStateOf(false) }
    var rejected by remember { mutableStateOf(false) }

    LaunchedEffect(resumed) {
        if (!resumed) return@LaunchedEffect
        while (isActive) {
            if (!busy) state = withContext(Dispatchers.IO) { ExperimentalAudioRepository.control.read() }
            delay(1_000)
        }
    }

    fun applyChange(change: () -> ExperimentalAudioResult) {
        if (!resumed || busy) return
        busy = true
        rejected = false
        scope.launch {
            try {
                val result = withContext(Dispatchers.IO) { change() }
                state = result.state
                rejected = !result.accepted
            } finally {
                busy = false
            }
        }
    }

    val status = when {
        state.reason == "dsp_recovery_required" -> R.string.experimental_audio_recovery_required
        rejected -> R.string.experimental_audio_rejected
        state.active != ExperimentalAudioMode.OFF && state.outputActive == ExperimentalAudioOutput.UPPER ->
            R.string.experimental_audio_output_upper_active
        state.active != ExperimentalAudioMode.OFF && state.outputActive == ExperimentalAudioOutput.LOWER ->
            R.string.experimental_audio_output_lower_active
        state.active == ExperimentalAudioMode.STEREO -> R.string.experimental_audio_stereo_active
        state.active == ExperimentalAudioMode.DUAL_MONO -> R.string.experimental_audio_dual_active
        state.requested == ExperimentalAudioMode.OFF -> R.string.experimental_audio_mode_off
        state.reason == "call" -> R.string.experimental_audio_paused_call
        state.reason == "route" -> R.string.experimental_audio_paused_route
        state.reason == "gain_error" -> R.string.experimental_audio_gain_error
        state.reason == "sync_waiting" -> R.string.experimental_audio_sync_waiting
        state.reason == "sync_error" -> R.string.experimental_audio_sync_error
        state.reason == "receiver_unavailable" -> R.string.experimental_audio_receiver_unavailable
        state.reason == "stereo_format" -> R.string.experimental_audio_stereo_format
        state.reason in setOf("hook_unobserved", "dsp_unavailable") -> R.string.experimental_audio_stereo_unverified
        state.reason == "idle" -> R.string.experimental_audio_waiting
        else -> R.string.experimental_audio_paused
    }
    ExperimentalAudioScreen(
        onBack = onBack,
        mode = state.requested,
        available = state.available,
        stereoAvailable = state.stereoAvailable,
        busy = busy,
        gainAvailable = state.gainAvailable,
        gainDb = state.gainDb,
        softwareGainAvailable = state.softwareGainAvailable,
        softwareGainDb = state.softwareGainDb,
        syncTrimAvailable = state.syncTrimAvailable,
        syncTrimMs = state.syncTrimMs,
        outputAvailable = state.outputAvailable,
        output = state.output,
        status = stringResource(status),
        unavailableReason = stringResource(when (state.reason) {
            "kernel_missing" -> R.string.experimental_audio_kernel_missing
            "calibration_missing" -> R.string.experimental_audio_calibration_missing
            else -> R.string.experimental_audio_unavailable
        }),
        onModeChange = { mode -> applyChange { ExperimentalAudioRepository.control.apply(mode) } },
        onGainChange = { gain -> applyChange { ExperimentalAudioRepository.control.applyGain(gain) } },
        onSoftwareGainChange = { gain -> applyChange { ExperimentalAudioRepository.control.applySoftwareGain(gain) } },
        onSyncTrimChange = { trim -> applyChange { ExperimentalAudioRepository.control.applySyncTrim(trim) } },
        onOutputChange = { output -> applyChange { ExperimentalAudioRepository.control.applyOutput(output) } },
    )
}
