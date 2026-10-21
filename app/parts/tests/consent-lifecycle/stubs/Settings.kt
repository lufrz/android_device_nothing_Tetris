// SPDX-License-Identifier: Apache-2.0
package android.provider

import android.content.ContentResolver

object Settings {
    object Global {
        const val DEVICE_PROVISIONED = "device_provisioned"
        fun getInt(resolver: ContentResolver, key: String, fallback: Int) =
            resolver.values[key] ?: fallback
        fun getUriFor(key: String) = "global/$key"
    }
    object Secure {
        const val USER_SETUP_COMPLETE = "user_setup_complete"
        fun getInt(resolver: ContentResolver, key: String, fallback: Int) =
            resolver.values[key] ?: fallback
        fun getUriFor(key: String) = "secure/$key"
    }
}
