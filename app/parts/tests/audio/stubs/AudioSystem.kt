// SPDX-License-Identifier: Apache-2.0
package android.media

object AudioSystem {
    val reads = mutableListOf<String>()
    val writes = mutableListOf<String>()
    var readAction: () -> String? = { null }
    var writeAction: (String) -> Int = { 0 }

    @JvmStatic fun getParameters(keys: String): String? {
        reads += keys
        return readAction()
    }
    @JvmStatic fun setParameters(parameters: String): Int {
        writes += parameters
        return writeAction(parameters)
    }
    fun reset() {
        reads.clear()
        writes.clear()
        readAction = { null }
        writeAction = { 0 }
    }
}
