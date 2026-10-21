// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.ui

import android.view.HapticFeedbackConstants
import androidx.compose.runtime.Composable
import androidx.compose.runtime.remember
import androidx.compose.ui.platform.LocalView

/** Call only when a user gesture crosses a discrete slider value. */
@Composable
fun rememberSliderHaptics(): () -> Unit {
    val view = LocalView.current
    return remember(view) {
        { view.performHapticFeedback(HapticFeedbackConstants.SEGMENT_TICK); Unit }
    }
}
