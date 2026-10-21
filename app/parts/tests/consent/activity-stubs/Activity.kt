// SPDX-License-Identifier: Apache-2.0
package androidx.activity

import android.content.Context
import android.content.Intent
import android.os.Bundle
import android.view.Window
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel

open class ComponentActivity : Context() {
    var intent = Intent()
    val window = Window()
    val onBackPressedDispatcher = OnBackPressedDispatcher()
    val testScope = CoroutineScope(SupervisorJob() + Dispatchers.Unconfined)
    val launched = mutableListOf<Intent>()
    @Volatile var finishCount = 0
    val isFinishing get() = finishCount > 0
    var content: (() -> Unit)? = null
    var edgeToEdge = false
    open fun onCreate(state: Bundle?) {}
    open fun onResume() {}
    open fun onPause() {}
    open fun onStop() {}
    open fun onDestroy() { testScope.cancel() }
    open fun onSaveInstanceState(state: Bundle) {}
    fun finish() { finishCount++ }
    fun startActivity(intent: Intent) { launched += intent }
    fun render() { content?.invoke() }
}
abstract class OnBackPressedCallback(var isEnabled: Boolean) {
    abstract fun handleOnBackPressed()
}
class OnBackPressedDispatcher {
    val callbacks = mutableListOf<OnBackPressedCallback>()
    fun addCallback(owner: ComponentActivity, callback: OnBackPressedCallback) { callbacks += callback }
}
fun ComponentActivity.enableEdgeToEdge() { edgeToEdge = true }
