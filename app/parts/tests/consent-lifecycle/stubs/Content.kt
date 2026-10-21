// SPDX-License-Identifier: Apache-2.0
package android.content

import android.database.ContentObserver

open class Context {
    var application: Context? = null
    open val applicationContext: Context get() = application ?: this
    val services = mutableMapOf<Class<*>, Any>()
    private val resolver = ContentResolver()
    val contentResolver: ContentResolver get() = if (application != null) applicationContext.contentResolver else resolver
    val starts = mutableListOf<Intent>()
    val attempts = mutableListOf<Intent>()
    val receivers = mutableListOf<ReceiverRegistration>()
    var failStart = false
    var dropStart = false
    @Suppress("UNCHECKED_CAST")
    fun <T> getSystemService(type: Class<T>): T? = applicationContext.services[type] as T?
    fun startActivity(intent: Intent) {
        if (application != null) { applicationContext.startActivity(intent); return }
        attempts += intent
        if (failStart) throw IllegalStateException("start denied")
        if (dropStart) return
        starts += intent
    }
    fun registerReceiver(receiver: BroadcastReceiver, filter: IntentFilter, flags: Int): Intent? {
        applicationContext.receivers += ReceiverRegistration(this, receiver, filter, flags)
        return null
    }
    fun unregisterReceiver(receiver: BroadcastReceiver) {
        check(applicationContext.receivers.any { it.receiver === receiver }) { "Receiver was not registered" }
        applicationContext.receivers.removeAll { it.receiver === receiver }
    }
    fun sendBroadcast(intent: Intent, senderUid: Int = when (intent.action) {
        Intent.ACTION_USER_PRESENT -> SYSTEM_UI_UID
        else -> SYSTEM_UID
    }) {
        // USER_PRESENT originates from the separately shared-UID SystemUI process. Protected
        // actions prevent third-party spoofing; they do not waive receiver export restrictions.
        if (intent.action in setOf(Intent.ACTION_USER_PRESENT, Intent.ACTION_USER_UNLOCKED) &&
                senderUid != SYSTEM_UID && senderUid != SYSTEM_UI_UID) {
            throw SecurityException("Only privileged callers may send protected unlock broadcasts")
        }
        applicationContext.receivers.toList().filter { intent.action in it.filter.actions }.forEach {
            if (it.flags and RECEIVER_EXPORTED == 0 && senderUid != SYSTEM_UID) return@forEach
            it.receiver.onReceive(it.context, intent)
        }
    }
    data class ReceiverRegistration(
        val context: Context,
        val receiver: BroadcastReceiver,
        val filter: IntentFilter,
        val flags: Int,
    )
    companion object {
        const val RECEIVER_EXPORTED = 2
        const val RECEIVER_NOT_EXPORTED = 4
        const val SYSTEM_UID = 1000
        // Test identities: SystemUI is a privileged app but not android.uid.system.
        const val SYSTEM_UI_UID = 10002
        const val THIRD_PARTY_UID = 20000
    }
}
class ContentResolver {
    val values = mutableMapOf<String, Int>()
    val observers = mutableListOf<Pair<String, ContentObserver>>()
    var onRegister: (() -> Unit)? = null
    fun registerContentObserver(uri: String, notifyForDescendants: Boolean, observer: ContentObserver) {
        observers += uri to observer
        onRegister?.invoke()
    }
    fun unregisterContentObserver(observer: ContentObserver) { observers.removeAll { it.second === observer } }
    fun notifyChange(uri: String) {
        observers.filter { it.first == uri }.map { it.second }.distinct().forEach { it.onChange(false) }
    }
    fun setAndNotify(key: String, value: Int, uri: String) {
        values[key] = value
        notifyChange(uri)
    }
}
class IntentFilter(action: String? = null) {
    val actions = mutableSetOf<String>()
    init { if (action != null) actions += action }
    fun addAction(action: String) { actions += action }
}
class ComponentName(context: Context, type: Class<*>)
class Intent(val action: String? = null) {
    var flags = 0
    fun addFlags(value: Int): Intent { flags = flags or value; return this }
    companion object {
        const val ACTION_LOCKED_BOOT_COMPLETED = "locked_boot"
        const val ACTION_BOOT_COMPLETED = "boot"
        const val ACTION_MY_PACKAGE_REPLACED = "replaced"
        const val ACTION_USER_PRESENT = "user_present"
        const val ACTION_USER_UNLOCKED = "user_unlocked"
        const val FLAG_ACTIVITY_NEW_TASK = 1
    }
}
abstract class BroadcastReceiver { abstract fun onReceive(context: Context, intent: Intent) }
