// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.adaptive
import org.lineageos.settings.tetris.ScreenProbe
object AdaptiveService { const val EXTRA_PROPOSAL_ID = "proposal_id" }
fun AdaptiveScreen(onBack: () -> Unit, proposalToReview: String?, onReviewRequestConsumed: () -> Unit) {
    ScreenProbe.showPage("adaptive", onBack)
    ScreenProbe.reviewId = proposalToReview
    ScreenProbe.consumeReview = onReviewRequestConsumed
}
