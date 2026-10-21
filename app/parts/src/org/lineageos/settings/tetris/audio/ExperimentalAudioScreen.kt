/*
 * SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris.audio

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.RadioButton
import androidx.compose.material3.Slider
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.runtime.Composable
import androidx.compose.runtime.getValue
import androidx.compose.runtime.setValue
import androidx.compose.runtime.mutableFloatStateOf
import androidx.compose.runtime.remember
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.lineageos.settings.tetris.R
import org.lineageos.settings.tetris.ui.PartsInfoCard
import org.lineageos.settings.tetris.ui.PartsPreferenceGroup
import org.lineageos.settings.tetris.ui.PartsPreference
import org.lineageos.settings.tetris.ui.PartsScaffold
import org.lineageos.settings.tetris.ui.PartsSwitchPreference
import org.lineageos.settings.tetris.ui.rememberPartsResumed
import org.lineageos.settings.tetris.ui.rememberSliderHaptics
import kotlin.math.roundToInt

/**
 * Displays the controller's state without persisting or optimistically changing it.
 * The containing Activity owns back navigation, including the system's predictive gesture.
 */
@Composable
fun ExperimentalAudioScreen(
    onBack: () -> Unit,
    onModeChange: (ExperimentalAudioMode) -> Unit,
    mode: ExperimentalAudioMode = ExperimentalAudioMode.OFF,
    available: Boolean = false,
    stereoAvailable: Boolean = false,
    busy: Boolean = false,
    status: String? = null,
    unavailableReason: String? = null,
    stereoUnavailableReason: String? = null,
    gainAvailable: Boolean = false,
    gainDb: Int = 0,
    onGainChange: (Int) -> Unit = {},
    softwareGainAvailable: Boolean = false,
    softwareGainDb: Int = 0,
    onSoftwareGainChange: (Int) -> Unit = {},
    syncTrimAvailable: Boolean = false,
    syncTrimMs: Int = 0,
    onSyncTrimChange: (Int) -> Unit = {},
    outputAvailable: Boolean = false,
    output: ExperimentalAudioOutput = ExperimentalAudioOutput.BOTH,
    onOutputChange: (ExperimentalAudioOutput) -> Unit = {},
) {
    val resumed = rememberPartsResumed()
    val dualEnabled = mode != ExperimentalAudioMode.OFF
    val stereoEnabled = mode == ExperimentalAudioMode.STEREO
    val interactive = resumed && !busy
    val separateOutput = dualEnabled && (stereoEnabled || output != ExperimentalAudioOutput.BOTH)
    val currentStatus = when {
        busy -> stringResource(R.string.experimental_audio_applying)
        !status.isNullOrBlank() -> status
        mode == ExperimentalAudioMode.STEREO -> stringResource(R.string.experimental_audio_mode_stereo)
        mode == ExperimentalAudioMode.DUAL_MONO -> stringResource(R.string.experimental_audio_mode_dual_mono)
        else -> stringResource(R.string.experimental_audio_mode_off)
    }

    PartsScaffold(
        title = stringResource(R.string.experimental_audio_group),
        onBack = onBack,
    ) { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                top = padding.calculateTopPadding() + 8.dp,
                bottom = padding.calculateBottomPadding() + 24.dp,
            ),
        ) {
            item("audio_controls") {
                PartsPreferenceGroup(title = stringResource(R.string.experimental_audio_group)) {
                    PartsSwitchPreference(
                        title = stringResource(R.string.experimental_audio_dual_speaker_title),
                        summary = stringResource(R.string.experimental_audio_dual_speaker_summary),
                        checked = dualEnabled,
                        // Keep the escape action available if the backend loses a capability.
                        enabled = interactive && (available || dualEnabled),
                        onCheckedChange = { enabled ->
                            if (interactive && (!enabled || available)) {
                                onModeChange(if (enabled) ExperimentalAudioMode.DUAL_MONO
                                    else ExperimentalAudioMode.OFF)
                            }
                        },
                    )
                    PartsSwitchPreference(
                        title = stringResource(R.string.experimental_audio_stereo_title),
                        summary = stringResource(R.string.experimental_audio_stereo_summary),
                        checked = stereoEnabled,
                        enabled = interactive && dualEnabled &&
                            (stereoEnabled || (available && stereoAvailable)),
                        onCheckedChange = { enabled ->
                            if (interactive && dualEnabled) {
                                if (enabled && available && stereoAvailable) {
                                    onModeChange(ExperimentalAudioMode.STEREO)
                                } else if (!enabled) {
                                    onModeChange(if (available) ExperimentalAudioMode.DUAL_MONO
                                        else ExperimentalAudioMode.OFF)
                                }
                            }
                        },
                    )
                }
            }
            if (outputAvailable) {
                item("audio_output") {
                    PartsPreferenceGroup(title = stringResource(R.string.experimental_audio_output_title)) {
                        ExperimentalAudioOutput.entries.forEach { choice ->
                            val enabled = interactive && dualEnabled && available && stereoAvailable
                            PartsPreference(
                                title = stringResource(when (choice) {
                                    ExperimentalAudioOutput.BOTH -> R.string.experimental_audio_output_both
                                    ExperimentalAudioOutput.UPPER -> R.string.experimental_audio_output_upper
                                    ExperimentalAudioOutput.LOWER -> R.string.experimental_audio_output_lower
                                }),
                                summary = stringResource(if (choice == ExperimentalAudioOutput.BOTH)
                                    R.string.experimental_audio_output_both_summary else R.string.experimental_audio_output_single_summary),
                                selected = output == choice,
                                enabled = enabled,
                                onClick = { if (enabled && choice != output) onOutputChange(choice) },
                                trailing = { RadioButton(selected = output == choice, onClick = null, enabled = enabled) },
                            )
                        }
                    }
                }
            }
            if (gainAvailable) {
                item("receiver_gain") {
                    AudioAdjustmentCard(
                        title = stringResource(R.string.experimental_audio_gain_title),
                        summary = stringResource(R.string.experimental_audio_gain_summary),
                        value = gainDb, enabled = interactive,
                        minimum = ExperimentalAudioState.MIN_GAIN_DB, maximum = ExperimentalAudioState.MAX_GAIN_DB,
                        valueResource = R.string.experimental_audio_gain_value,
                        resetResource = R.string.experimental_audio_gain_reset,
                        onChange = onGainChange,
                    )
                }
            }
            if (softwareGainAvailable) {
                item("receiver_software_gain") {
                    AudioAdjustmentCard(
                        title = stringResource(R.string.experimental_audio_software_gain_title),
                        summary = stringResource(R.string.experimental_audio_software_gain_summary),
                        value = softwareGainDb, enabled = interactive && separateOutput && output != ExperimentalAudioOutput.LOWER,
                        minimum = ExperimentalAudioState.MIN_SOFTWARE_GAIN_DB, maximum = ExperimentalAudioState.MAX_SOFTWARE_GAIN_DB,
                        valueResource = R.string.experimental_audio_gain_value,
                        resetResource = R.string.experimental_audio_gain_reset,
                        onChange = onSoftwareGainChange,
                    )
                }
            }
            if (syncTrimAvailable) {
                item("receiver_sync_trim") {
                    AudioAdjustmentCard(
                        title = stringResource(R.string.experimental_audio_sync_trim_title),
                        summary = stringResource(R.string.experimental_audio_sync_trim_summary),
                        value = syncTrimMs, enabled = interactive && stereoEnabled && output == ExperimentalAudioOutput.BOTH,
                        minimum = ExperimentalAudioState.MIN_SYNC_TRIM_MS, maximum = ExperimentalAudioState.MAX_SYNC_TRIM_MS,
                        valueResource = R.string.experimental_audio_sync_trim_value,
                        resetResource = R.string.experimental_audio_sync_trim_reset,
                        onChange = onSyncTrimChange,
                    )
                }
            }
            item("audio_status") {
                PartsInfoCard(
                    title = stringResource(R.string.experimental_audio_status_title),
                    body = currentStatus,
                )
            }
            if (!available) {
                item("audio_unavailable") {
                    PartsInfoCard(body = unavailableReason?.takeIf { it.isNotBlank() }
                        ?: stringResource(R.string.experimental_audio_unavailable))
                }
            } else if (!stereoAvailable) {
                item("stereo_unavailable") {
                    PartsInfoCard(body = stereoUnavailableReason?.takeIf { it.isNotBlank() }
                        ?: stringResource(R.string.experimental_audio_stereo_unavailable))
                }
            } else if (!dualEnabled) {
                item("stereo_requires_dual") {
                    PartsInfoCard(body = stringResource(R.string.experimental_audio_stereo_requires_dual))
                }
            }
            item("audio_help") {
                PartsInfoCard(body = stringResource(R.string.experimental_audio_help))
            }
        }
    }
}

@Composable
private fun AudioAdjustmentCard(title: String, summary: String, value: Int, enabled: Boolean,
        minimum: Int, maximum: Int, valueResource: Int, resetResource: Int, onChange: (Int) -> Unit) {
    var preview by remember(value, enabled) { mutableFloatStateOf(value.toFloat()) }
    val tick = rememberSliderHaptics()
    PartsPreferenceGroup(title = title) {
        Surface(color = MaterialTheme.colorScheme.surfaceBright) {
            Column(Modifier.fillMaxWidth().padding(20.dp)) {
                Row(Modifier.fillMaxWidth(), horizontalArrangement = Arrangement.SpaceBetween,
                    verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(valueResource, preview.roundToInt()), style = MaterialTheme.typography.titleMedium)
                    TextButton(enabled = enabled && value != 0, onClick = { onChange(0) }) {
                        Text(stringResource(resetResource))
                    }
                }
                Slider(
                    value = preview,
                    onValueChange = {
                        val next = it.roundToInt().coerceIn(minimum, maximum).toFloat()
                        if (enabled && next != preview) {
                            preview = next
                            tick()
                        }
                    },
                    onValueChangeFinished = {
                        val next = preview.roundToInt()
                        if (enabled && next != value) onChange(next)
                    },
                    enabled = enabled,
                    valueRange = minimum.toFloat()..maximum.toFloat(),
                    steps = maximum - minimum - 1,
                )
                Text(summary, style = MaterialTheme.typography.bodyMedium,
                    color = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
    }
}
