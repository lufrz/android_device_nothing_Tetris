/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.battery

import java.io.File

/** Kernel owns unplug/reboot expiry. No preference, boot reapply or background writer is needed. */
internal object ChargeBypassSession {
    private const val ROOT = "/sys/devices/platform/charger"
    val manager = ChargeBypassManager(object : ChargeBypassPort {
        private val request = File("$ROOT/charge_bypass")
        private val status = File("$ROOT/charge_bypass_status")
        override fun exists() = request.exists()
        override fun writable() = request.canWrite()
        override fun readRequested() = request.readText()
        override fun readStatus() = status.readText()
        override fun writeRequested(requested: Boolean) = request.writeText(if (requested) "1\n" else "0\n")
    })
}
