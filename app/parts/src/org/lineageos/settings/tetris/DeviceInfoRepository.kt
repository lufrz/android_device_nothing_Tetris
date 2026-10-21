/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris

import android.app.ActivityManager
import android.content.Context
import android.os.storage.StorageManager

internal data class DeviceCapacity(val ramBytes: Long?, val storageBytes: Long?)

internal class DeviceInfoRepository(context: Context) {
    private val context = context.applicationContext

    // Read memory and storage capacity once for the overview.
    fun readCapacity(): DeviceCapacity {
        val ram = runCatching {
            val manager = context.getSystemService(ActivityManager::class.java)
                ?: return@runCatching null
            ActivityManager.MemoryInfo().also(manager::getMemoryInfo).let { info ->
                // advertisedMem can round the 6 GB variant up to 8 GB. Use the
                // kernel-visible RAM, consistently with the memory management page.
                info.totalMem.takeIf { it > 0 }
            }
        }.getOrNull()
        val storage = runCatching {
            StorageManager.getPrimaryStoragePathAndSize().second.takeIf { it > 0 }
        }.getOrNull()
        return DeviceCapacity(ram, storage)
    }
}
