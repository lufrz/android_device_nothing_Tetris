/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris

import android.os.Build
import android.text.format.Formatter
import androidx.compose.foundation.Image
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.BoxWithConstraints
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.heightIn
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.BatteryFull
import androidx.compose.material.icons.filled.DeveloperBoard
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.PhotoCamera
import androidx.compose.material.icons.filled.SdStorage
import androidx.compose.material.icons.filled.Smartphone
import androidx.compose.material3.Icon
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.Surface
import androidx.compose.material3.Text
import androidx.compose.runtime.Composable
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Brush
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.layout.ContentScale
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.platform.LocalDensity
import androidx.compose.ui.res.painterResource
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.semantics.heading
import androidx.compose.ui.semantics.semantics
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextAlign
import androidx.compose.ui.unit.dp
import org.lineageos.settings.tetris.ui.PartsGroupShape
import org.lineageos.settings.tetris.ui.PartsPageGutter

@Composable
internal fun DeviceHero(deviceName: String) {
    Surface(
        modifier = Modifier
            .fillMaxWidth()
            .padding(horizontal = PartsPageGutter, vertical = 6.dp),
        shape = PartsGroupShape,
        color = Color(0xFFFFDCE1),
        contentColor = Color(0xFF542638),
    ) {
        Box(
            modifier = Modifier.fillMaxWidth().heightIn(min = 180.dp),
            contentAlignment = Alignment.Center,
        ) {
            Image(
                painter = painterResource(R.drawable.tetris_device_background),
                contentDescription = null,
                contentScale = ContentScale.Crop,
                modifier = Modifier.matchParentSize(),
            )
            // Keep the text readable over the artwork in both system themes.
            Box(
                Modifier.matchParentSize().background(
                    Brush.horizontalGradient(
                        listOf(Color(0xB3FFF4ED), Color(0x99FFF4ED)),
                    ),
                ),
            )
            DeviceHeading(deviceName, Modifier.fillMaxWidth().padding(24.dp))
        }
    }
}

@Composable
private fun DeviceHeading(deviceName: String, modifier: Modifier = Modifier) {
    Column(
        modifier,
        verticalArrangement = Arrangement.spacedBy(6.dp),
        horizontalAlignment = Alignment.CenterHorizontally,
    ) {
        Text(
            text = deviceName,
            style = MaterialTheme.typography.headlineSmall,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth().semantics { heading() },
        )
        Text(
            text = stringResource(R.string.parts_device_chip),
            style = MaterialTheme.typography.bodyMedium,
            color = Color(0xFF67374A),
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth(),
        )
        Text(
            text = stringResource(R.string.parts_device_android, Build.VERSION.RELEASE),
            style = MaterialTheme.typography.labelLarge,
            color = Color(0xFF67374A),
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth(),
        )
        Text(
            text = stringResource(
                R.string.parts_device_maintainer,
                stringResource(R.string.parts_device_maintainer_name),
            ),
            style = MaterialTheme.typography.labelLarge,
            textAlign = TextAlign.Center,
            modifier = Modifier.fillMaxWidth().padding(top = 12.dp),
        )
    }
}

@Composable
internal fun DeviceSpecifications(capacity: DeviceCapacity?) {
    val context = LocalContext.current
    val unavailable = stringResource(R.string.parts_spec_unavailable)
    val loading = stringResource(R.string.parts_spec_loading)
    val ram = if (capacity == null) loading else capacity.ramBytes?.let {
        Formatter.formatFileSize(context, it, Formatter.FLAG_IEC_UNITS or Formatter.FLAG_SHORTER)
    } ?: unavailable
    val storage = if (capacity == null) loading else capacity.storageBytes?.let {
        Formatter.formatShortFileSize(context, it)
    } ?: unavailable
    val specs = listOf(
        DeviceSpec(stringResource(R.string.parts_spec_processor),
            stringResource(R.string.parts_spec_soc), Icons.Filled.DeveloperBoard),
        DeviceSpec(stringResource(R.string.parts_spec_camera),
            stringResource(R.string.parts_spec_camera_tetris), Icons.Filled.PhotoCamera),
        DeviceSpec(stringResource(R.string.parts_spec_ram), ram, Icons.Filled.Memory),
        DeviceSpec(stringResource(R.string.parts_spec_storage), storage, Icons.Filled.SdStorage),
        DeviceSpec(stringResource(R.string.parts_spec_battery),
            stringResource(R.string.parts_spec_battery_capacity), Icons.Filled.BatteryFull),
        DeviceSpec(stringResource(R.string.parts_spec_screen),
            stringResource(R.string.parts_spec_display), Icons.Filled.Smartphone),
    )
    BoxWithConstraints(
        Modifier.fillMaxWidth().padding(horizontal = PartsPageGutter, vertical = 10.dp),
    ) {
        val columns = if (maxWidth < 280.dp || LocalDensity.current.fontScale > 1.5f) 1 else 2
        Column(verticalArrangement = Arrangement.spacedBy(20.dp)) {
            specs.chunked(columns).forEach { row ->
                Row(horizontalArrangement = Arrangement.spacedBy(24.dp)) {
                    row.forEach { spec -> DeviceSpecItem(spec, Modifier.weight(1f)) }
                }
            }
        }
    }
}

private data class DeviceSpec(val title: String, val value: String, val icon: ImageVector)

@Composable
private fun DeviceSpecItem(spec: DeviceSpec, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier.semantics(mergeDescendants = true) {},
        verticalArrangement = Arrangement.spacedBy(6.dp),
    ) {
        Icon(
            imageVector = spec.icon,
            contentDescription = null,
            modifier = Modifier.size(22.dp),
            tint = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Text(
            text = spec.title,
            style = MaterialTheme.typography.titleMedium,
            fontWeight = FontWeight.Medium,
        )
        Text(
            text = spec.value,
            style = MaterialTheme.typography.bodyMedium,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
    }
}
