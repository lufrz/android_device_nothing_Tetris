// SPDX-License-Identifier: Apache-2.0
package android.app

object ActivityManager {
    var user = 0
    fun getCurrentUser(): Int = user
}
class KeyguardManager(var isKeyguardLocked: Boolean = false)
