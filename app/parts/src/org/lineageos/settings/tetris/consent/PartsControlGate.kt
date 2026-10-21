/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.consent

/** Process gate complements the property checked by native persisted features. */
internal object PartsControlGate {
    @Volatile var allowed = false
    private val mutations = Any()

    fun <T> runIfAllowed(action: () -> T): T? = synchronized(mutations) {
        if (allowed) action() else null
    }

    /** Worker only: wait for old callbacks and their rollbacks before resetting values. */
    fun awaitPendingMutations() { synchronized(mutations) { } }
}
