// SPDX-License-Identifier: Apache-2.0
package android.util
object Log {
    fun w(tag: String, message: String, error: Throwable? = null) = 0
    fun i(tag: String, message: String) = 0
}
