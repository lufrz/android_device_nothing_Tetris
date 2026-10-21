// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent

import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.WindowInsetsSides
import androidx.compose.foundation.layout.consumeWindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.only
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.windowInsetsPadding
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.material3.Button
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.Scaffold
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.material3.TopAppBar
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.runtime.Composable
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.LiveRegionMode
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.liveRegion
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.unit.dp
import org.lineageos.settings.tetris.R

/** Presentation only: the host owns the decision, elapsed time and system navigation. */
@OptIn(ExperimentalMaterial3Api::class)
@Composable
internal fun TermsScreen(
    mandatory: Boolean,
    remainingSeconds: Int,
    busy: Boolean,
    error: Boolean,
    onAccept: () -> Unit,
    onDecline: () -> Unit,
    onBack: () -> Unit,
    preparing: Boolean = false,
) {
    Scaffold(
        topBar = {
            TopAppBar(
                title = { Text(stringResource(R.string.parts_title)) },
                navigationIcon = {
                    if (!mandatory) {
                        IconButton(onClick = onBack) {
                            Icon(
                                imageVector = Icons.AutoMirrored.Filled.ArrowBack,
                                contentDescription = stringResource(R.string.navigate_up),
                            )
                        }
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surfaceContainer,
                ),
            )
        },
        bottomBar = {
            Surface(
                color = MaterialTheme.colorScheme.surfaceContainer,
                tonalElevation = 3.dp,
            ) {
                Column(
                    modifier = Modifier
                        .fillMaxWidth()
                        .windowInsetsPadding(
                            WindowInsets.safeDrawing.only(
                                WindowInsetsSides.Horizontal + WindowInsetsSides.Bottom,
                            ),
                        )
                        .padding(horizontal = 20.dp, vertical = 12.dp),
                    verticalArrangement = Arrangement.spacedBy(8.dp),
                ) {
                    if (error) {
                        Text(
                            text = stringResource(R.string.terms_save_error),
                            style = MaterialTheme.typography.bodyMedium,
                            color = MaterialTheme.colorScheme.error,
                            modifier = Modifier.semantics { liveRegion = LiveRegionMode.Polite },
                        )
                    }
                    Button(
                        onClick = onAccept,
                        enabled = remainingSeconds <= 0 && !preparing && !busy && !error,
                        modifier = Modifier.fillMaxWidth(),
                        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 14.dp),
                    ) {
                        Text(
                            text = when {
                                remainingSeconds > 0 -> stringResource(
                                    R.string.terms_accept_countdown, remainingSeconds,
                                )
                                preparing -> stringResource(R.string.terms_preparing)
                                busy -> stringResource(R.string.terms_applying)
                                else -> stringResource(R.string.terms_accept)
                            },
                        )
                    }
                    OutlinedButton(
                        onClick = onDecline,
                        enabled = !busy,
                        modifier = Modifier.fillMaxWidth(),
                        contentPadding = PaddingValues(horizontal = 16.dp, vertical = 14.dp),
                    ) {
                        Text(stringResource(R.string.terms_decline))
                    }
                }
            }
        },
        containerColor = MaterialTheme.colorScheme.surfaceContainer,
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { padding ->
        LazyColumn(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .consumeWindowInsets(padding),
            contentPadding = PaddingValues(horizontal = 20.dp, vertical = 16.dp),
            verticalArrangement = Arrangement.spacedBy(20.dp),
        ) {
            item("title") {
                Text(
                    text = stringResource(R.string.terms_title),
                    style = MaterialTheme.typography.headlineMedium,
                    modifier = Modifier.semantics { heading() },
                )
            }
            item("introduction") {
                Surface(
                    shape = MaterialTheme.shapes.large,
                    color = MaterialTheme.colorScheme.primaryContainer,
                    contentColor = MaterialTheme.colorScheme.onPrimaryContainer,
                ) {
                    Text(
                        text = stringResource(
                            if (mandatory) R.string.terms_introduction_required
                            else R.string.terms_introduction_manual,
                        ),
                        modifier = Modifier.padding(16.dp),
                        style = MaterialTheme.typography.bodyLarge,
                    )
                }
            }
            item("choice") {
                TermsSection(R.string.terms_choice_title, R.string.terms_choice_body)
            }
            item("performance") {
                TermsSection(R.string.terms_performance_title, R.string.terms_performance_body)
            }
            item("memory_adaptive") {
                TermsSection(R.string.terms_memory_title, R.string.terms_memory_body)
            }
            item("audio") {
                TermsSection(R.string.terms_audio_title, R.string.terms_audio_body)
            }
            item("network") {
                TermsSection(R.string.terms_network_title, R.string.terms_network_body)
            }
            item("data") {
                TermsSection(R.string.terms_data_title, R.string.terms_data_body)
            }
            item("rom") {
                TermsSection(R.string.terms_rom_title, R.string.terms_rom_body)
            }
            item("rights") {
                TermsSection(R.string.terms_rights_title, R.string.terms_rights_body)
            }
        }
    }
}

@Composable
private fun TermsSection(title: Int, body: Int) {
    Column(verticalArrangement = Arrangement.spacedBy(8.dp)) {
        Text(
            text = stringResource(title),
            style = MaterialTheme.typography.titleMedium,
            color = MaterialTheme.colorScheme.primary,
            modifier = Modifier.semantics { heading() },
        )
        Text(
            text = stringResource(body),
            style = MaterialTheme.typography.bodyLarge,
            color = MaterialTheme.colorScheme.onSurface,
        )
    }
}
