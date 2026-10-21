// SPDX-License-Identifier: Apache-2.0
package android.database

import android.os.Handler

abstract class ContentObserver(handler: Handler?) {
    open fun onChange(selfChange: Boolean) = Unit
}
