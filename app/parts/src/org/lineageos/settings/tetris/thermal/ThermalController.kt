/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.thermal

import android.os.SystemProperties

internal data class ThermalState(
    val mode: String,
    val error: String,
    val errno: Int,
    val profile: String,
    val resultId: String,
) {
    val disabled: Boolean get() = mode == "disabled"
    val confirmed: Boolean get() = mode == "disabled" || mode == "enabled"
}

internal object ThermalController {
    fun read() = ThermalState(
        mode = SystemProperties.get("vendor.tetrisparts.thermal_state"),
        error = SystemProperties.get("vendor.tetrisparts.thermal_error", "none"),
        errno = SystemProperties.getInt("vendor.tetrisparts.thermal_errno", 0),
        profile = SystemProperties.get("vendor.tetrisparts.thermal_profile"),
        resultId = SystemProperties.get("vendor.tetrisparts.thermal_result_id"),
    )

    fun request(disabled: Boolean) {
        SystemProperties.set("sys.tetrisparts.thermal_disabled", if (disabled) "1" else "0")
    }
}
