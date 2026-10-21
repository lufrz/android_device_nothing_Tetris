/* SPDX-License-Identifier: Apache-2.0 */
package org.lineageos.settings.tetris.consent

import android.content.Context
import android.os.SystemProperties
import android.os.UserHandle
import org.lineageos.settings.tetris.network.TcpControlSession

internal object TermsConsent {
    // Increase only when the terms change, independently of ROM/APK releases.
    private const val REVISION = 1
    // Increase only when an explicit request requires presenting unchanged terms again.
    private const val PROMPT_GENERATION = 0
    private const val PROPERTY = "sys.tetrisparts.consent"
    private const val PREFERENCES = "terms_consent"
    private const val ACCEPTED = "accepted_epoch"
    private const val PRESENTED = "presented_epoch"
    private const val IDENTITY_VERSION = "identity_version"

    private fun preferences(context: Context) = context.createDeviceProtectedStorageContext()
        .getSharedPreferences(PREFERENCES, Context.MODE_PRIVATE)

    @Suppress("UNUSED_PARAMETER")
    fun epoch(context: Context): String = "terms:$REVISION/request:$PROMPT_GENERATION"

    private fun state(context: Context): TermsState {
        val prefs = preferences(context)
        val saved = TermsState(prefs.getString(ACCEPTED, "").orEmpty(),
            prefs.getString(PRESENTED, "").orEmpty())
        if (prefs.getInt(IDENTITY_VERSION, 0) == 1) return saved
        val migrated = saved.migrateLegacy(epoch(context))
        check(prefs.edit().putString(ACCEPTED, migrated.accepted)
            .putString(PRESENTED, migrated.presented).putInt(IDENTITY_VERSION, 1).commit())
        return migrated
    }

    @Synchronized fun isAccepted(context: Context): Boolean =
        UserHandle.myUserId() == UserHandle.USER_SYSTEM &&
            runCatching { state(context).permits(epoch(context)) }.getOrDefault(false)

    /** Boot restores the volatile gate from the saved decision for these terms. */
    @Synchronized fun synchronize(context: Context): Boolean {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM) {
            PartsControlGate.allowed = false
            return false
        }
        val accepted = isAccepted(context)
        if (!accepted) PartsControlGate.allowed = false
        val value = if (accepted) "1" else "0"
        val permitted = runCatching {
            if (SystemProperties.get(PROPERTY, "0") != value) {
                val cleared = TcpControlSession.clearPendingRequests()
                if (accepted) check(cleared)
                SystemProperties.set(PROPERTY, value)
                // A failed network cleanup must never leave the audio gate open on revocation.
                check(cleared)
            }
            accepted
        }.getOrDefault(false)
        PartsControlGate.allowed = permitted
        return permitted
    }

    @Synchronized fun shouldPromptAutomatically(context: Context): Boolean =
        UserHandle.myUserId() == UserHandle.USER_SYSTEM &&
            runCatching { state(context).needsPresentation(epoch(context)) }.getOrDefault(false)

    @Synchronized fun markPresented(context: Context): Boolean =
        runCatching {
            state(context)
            preferences(context).edit().putString(PRESENTED, epoch(context)).commit()
        }.getOrDefault(false)

    /** A window can only accept the terms and explicit request it actually displayed. */
    @Synchronized fun decide(context: Context, displayedEpoch: String, accept: Boolean): Boolean {
        if (UserHandle.myUserId() != UserHandle.USER_SYSTEM || epoch(context) != displayedEpoch) return false
        if (!accept) PartsControlGate.allowed = false
        val next = runCatching { state(context).decide(displayedEpoch, accept) }
            .getOrNull() ?: return false
        if (!preferences(context).edit().putString(ACCEPTED, next.accepted)
                .putString(PRESENTED, next.presented).commit()) return false
        if (accept && !synchronize(context)) {
            preferences(context).edit().remove(ACCEPTED).commit()
            return false
        }
        if (!accept) synchronize(context)
        return true
    }
}
