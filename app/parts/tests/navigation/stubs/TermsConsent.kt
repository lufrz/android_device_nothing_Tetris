// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent

import android.content.Intent

object TermsConsent {
    var accepted = true
    fun synchronize(context: Any) = accepted
}
class TermsActivity {
    companion object {
        fun intent(context: Any, mandatory: Boolean) = Intent(context, TermsActivity::class.java)
    }
}
