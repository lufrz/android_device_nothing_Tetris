// SPDX-License-Identifier: Apache-2.0
package android.view

class Window {
    val insetsController: WindowInsetsController? = WindowInsetsController()
}
class WindowInsetsController {
    val hidden = mutableListOf<Int>()
    val shown = mutableListOf<Int>()
    fun hide(types: Int) { hidden += types }
    fun show(types: Int) { shown += types }
}
object WindowInsets {
    object Type {
        fun navigationBars() = 2
        fun statusBars() = 1
    }
}
