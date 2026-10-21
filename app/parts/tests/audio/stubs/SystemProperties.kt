// SPDX-License-Identifier: Apache-2.0
package android.os

object SystemProperties {
    val values = mutableMapOf<String, String>()
    val writes = mutableListOf<Pair<String, String>>()
    var setFailure: ((String, String) -> Throwable?)? = null

    @JvmStatic fun get(key: String): String = values[key].orEmpty()
    @JvmStatic fun get(key: String, default: String): String = values[key] ?: default
    @JvmStatic fun getInt(key: String, default: Int): Int = values[key]?.toIntOrNull() ?: default
    @JvmStatic fun set(key: String, value: String) {
        writes += key to value
        setFailure?.invoke(key, value)?.let { throw it }
        values[key] = value
    }
    fun reset() {
        org.lineageos.settings.tetris.consent.PartsControlGate.allowed = true
        values.clear()
        writes.clear()
        setFailure = null
    }
}
