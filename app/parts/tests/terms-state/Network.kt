// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.network
object TcpControlSession {
    var clearCount = 0
    var clearSucceeds = true
    fun clearPendingRequests(): Boolean { clearCount++; return clearSucceeds }
}
