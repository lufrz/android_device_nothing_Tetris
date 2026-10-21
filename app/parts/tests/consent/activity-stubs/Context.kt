// SPDX-License-Identifier: Apache-2.0
package android.content

import android.app.StatusBarManager

open class Context {
    val barManager = StatusBarManager()
    val applicationContext: Context get() = this
    @Suppress("UNCHECKED_CAST")
    fun <T> getSystemService(type: Class<T>): T? =
        if (type == StatusBarManager::class.java) barManager as T else null
}

class Intent() {
    var destination: Class<*>? = null
    private val extras = mutableMapOf<String, Any>()
    constructor(context: Context, destination: Class<*>) : this() {
        this.destination = destination
    }
    fun putExtra(key: String, value: Boolean) = apply { extras[key] = value }
    fun getBooleanExtra(key: String, default: Boolean) = extras[key] as? Boolean ?: default
}
