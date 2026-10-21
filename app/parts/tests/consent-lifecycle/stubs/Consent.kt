// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.consent

import android.content.Context
import android.content.Intent

object TermsConsent {
    var accepted = false
    var shown = false
    var synchronizations = 0
    fun synchronize(context: Context): Boolean { synchronizations++; return accepted }
    fun shouldPromptAutomatically(context: Context) = !accepted && !shown
}
object TermsActivity {
    fun intent(context: Context, mandatory: Boolean): Intent {
        check(mandatory)
        return Intent("terms")
    }
}
