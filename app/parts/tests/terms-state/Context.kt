// SPDX-License-Identifier: Apache-2.0
package android.content
class PackageInfo(var longVersionCode: Long = 11, var lastUpdateTime: Long = 100L)
class PackageManager {
    val info = PackageInfo()
    fun getPackageInfo(name: String, flags: Int) = info
}
class Preferences {
    val values = mutableMapOf<String, Any>()
    var writable = true
    fun getString(key: String, default: String): String? = (values[key] as? String) ?: default
    fun getInt(key: String, default: Int): Int = (values[key] as? Int) ?: default
    fun edit() = Editor(this)
    class Editor(private val store: Preferences) {
        private val edits = mutableMapOf<String, Any?>()
        fun putString(key: String, value: String) = apply { edits[key] = value }
        fun putInt(key: String, value: Int) = apply { edits[key] = value }
        fun remove(key: String) = apply { edits[key] = null }
        fun commit(): Boolean {
            if (!store.writable) return false
            edits.forEach { (key, value) -> if (value == null) store.values.remove(key) else store.values[key] = value }
            return true
        }
    }
}
class Context {
    val packageManager = PackageManager()
    val packageName = "org.lineageos.settings.tetris"
    val prefs = Preferences()
    fun createDeviceProtectedStorageContext() = this
    fun getSharedPreferences(name: String, mode: Int) = prefs
    companion object { const val MODE_PRIVATE = 0 }
}
