/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.consent

/** Acceptance and automatic presentation are deliberately independent. */
internal data class TermsState(
    val accepted: String = "",
    val presented: String = "",
) {
    fun permits(epoch: String) = epoch.isNotEmpty() && accepted == epoch
    fun needsPresentation(epoch: String) = !permits(epoch) && presented != epoch
    fun shown(epoch: String) = copy(presented = epoch)
    fun decide(epoch: String, accept: Boolean) = TermsState(if (accept) epoch else "", epoch)

    fun migrateLegacy(currentEpoch: String): TermsState {
        // All build-dependent hashes shipped with revision 1. Never promote one to a
        // later revision or explicit request; preserve both acceptance and refusal.
        fun identity(value: String) = when {
            LEGACY_HASH.matches(value) -> "terms:1/request:0"
            STABLE_IDENTITY.matches(value) -> value
            else -> ""
        }
        val accepted = identity(this.accepted)
        val presented = identity(this.presented).ifEmpty { accepted.ifEmpty { currentEpoch } }
        // A fresh installation requires manual consent, but does not open a window.
        return TermsState(accepted, presented)
    }

    private companion object {
        val LEGACY_HASH = Regex("[0-9a-f]{64}")
        val STABLE_IDENTITY = Regex("terms:[0-9]+/request:[0-9]+")
    }
}

internal object TermsCountdown {
    const val DURATION_MS = 20_000L

    fun deadline(now: Long, saved: Long?): Long =
        saved?.takeIf { it >= 0 && it <= now + DURATION_MS } ?: (now + DURATION_MS)

    fun remaining(deadline: Long, now: Long): Int =
        ((deadline - now).coerceIn(0L, DURATION_MS) + 999L).div(1000L).toInt()
}
