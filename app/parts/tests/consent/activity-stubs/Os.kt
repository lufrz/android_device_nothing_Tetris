// SPDX-License-Identifier: Apache-2.0
package android.os

class Bundle {
    private val values = mutableMapOf<String, Any>()
    fun putString(key: String, value: String) { values[key] = value }
    fun getString(key: String): String? = values[key] as? String
    fun putLong(key: String, value: Long) { values[key] = value }
    fun getLong(key: String): Long = values[key] as? Long ?: 0L
}
object SystemClock {
    @Volatile var now = 1000L
    fun elapsedRealtime() = now
}
object UserHandle {
    const val USER_SYSTEM = 0
    var user = USER_SYSTEM
    fun myUserId() = user
}
