/* Copyright (C) 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris.network

import android.os.SystemProperties
import java.io.File
import org.lineageos.settings.tetris.consent.PartsControlGate

/** Keep the initial value and ownership across recreation of the network screen. */
internal object TcpControlSession {
    /** Neutralize one-shot commands before either edge of the consent gate. */
    fun clearPendingRequests(): Boolean = runCatching {
        val keys = listOf("sys.tetrisparts.tcp_congestion", "sys.tetrisparts.tcp_restore")
        keys.forEach { SystemProperties.set(it, "off") }
        keys.all { SystemProperties.get(it) == "off" }
    }.getOrDefault(false)

    val manager = TcpControlManager(object : TcpControlPort {
        override fun clearPendingRequests() = TcpControlSession.clearPendingRequests()
        override fun readCurrent() = File("$SYSCTL/tcp_congestion_control").readText()
        override fun readAvailable() = File("$SYSCTL/tcp_available_congestion_control").readText()
        override fun controlReady() = PartsControlGate.allowed &&
            SystemProperties.get("sys.tetrisparts.consent", "0") == "1" &&
            SystemProperties.get("sys.tetrisparts.tcp_ready") == "1"
        override fun request(algorithm: String) {
            checkNotNull(PartsControlGate.runIfAllowed {
                check(SystemProperties.get("sys.tetrisparts.consent", "0") == "1")
                SystemProperties.set("sys.tetrisparts.tcp_congestion", algorithm)
            })
        }
        override fun restoreAfterConsent(algorithm: String) {
            SystemProperties.set("sys.tetrisparts.tcp_restore", algorithm)
        }
        override fun readBootAlgorithm() = SystemProperties.get(BOOT_ALGORITHM, "off")
        override fun saveBootAlgorithm(algorithm: String) {
            if (algorithm == "off") SystemProperties.set(BOOT_ALGORITHM, "off")
            else checkNotNull(PartsControlGate.runIfAllowed {
                SystemProperties.set(BOOT_ALGORITHM, algorithm)
            })
        }
    })

    private const val SYSCTL = "/proc/sys/net/ipv4"
    private const val BOOT_ALGORITHM = "persist.sys.tetrisparts.tcp_congestion"
}
