// SPDX-License-Identifier: Apache-2.0
package android.os
object Build { const val TIME = 1000L }
object UserHandle { const val USER_SYSTEM = 0; var user = 0; fun myUserId() = user }
object SystemProperties {
    val values = mutableMapOf<String, String>()
    var fail = false
    fun get(key: String, default: String) = values[key] ?: default
    fun set(key: String, value: String) { check(!fail); values[key] = value }
}
