// SPDX-License-Identifier: Apache-2.0
package android.widget
import android.content.Context
class Toast {
    fun show() { shown++ }
    companion object {
        const val LENGTH_LONG = 1
        @Volatile var shown = 0
        fun makeText(context: Context, text: Int, duration: Int) = Toast()
    }
}
