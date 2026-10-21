// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris

import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Speaker
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.unit.dp
import org.lineageos.settings.tetris.ui.PartsPreference
import org.lineageos.settings.tetris.ui.PartsPreferenceGroup
import org.lineageos.settings.tetris.ui.PartsScaffold

@Composable
fun ExperimentalScreen(onBack: () -> Unit, onAudio: () -> Unit) {
    PartsScaffold(title = stringResource(R.string.experimental_audio_title), onBack = onBack) { padding ->
        LazyColumn(
            modifier = Modifier.fillMaxSize(),
            contentPadding = PaddingValues(
                top = padding.calculateTopPadding() + 6.dp,
                bottom = padding.calculateBottomPadding() + 24.dp,
            ),
        ) {
            item("audio") {
                PartsPreferenceGroup {
                    PartsPreference(
                        title = stringResource(R.string.experimental_audio_group),
                        summary = stringResource(R.string.experimental_audio_home_summary),
                        icon = Icons.Filled.Speaker,
                        onClick = onAudio,
                    )
                }
            }
        }
    }
}
