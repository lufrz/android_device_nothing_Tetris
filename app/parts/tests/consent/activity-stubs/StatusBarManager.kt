// SPDX-License-Identifier: Apache-2.0
package android.app

class StatusBarManager {
    val primary = mutableListOf<Int>()
    val secondary = mutableListOf<Int>()
    var throwOnPrimary = false
    fun disable(flags: Int) {
        primary += flags
        if (throwOnPrimary) throw SecurityException("Test status-bar failure")
    }
    fun disable2(flags: Int) { secondary += flags }
    companion object {
        const val DISABLE_NONE = 0
        const val DISABLE_EXPAND = 0x00010000
        const val DISABLE_HOME = 0x00200000
        const val DISABLE_RECENT = 0x01000000
        const val DISABLE_BACK = 0x00400000
        const val DISABLE_SEARCH = 0x02000000
        const val DISABLE2_NONE = 0
        const val DISABLE2_QUICK_SETTINGS = 1
        const val DISABLE2_NOTIFICATION_SHADE = 4
    }
}
