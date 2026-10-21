// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.audio

import java.io.File

/** Parse the reports emitted by the production proxy in its host lifecycle tests. */
fun main(args: Array<String>) {
    val reports = File(args.single()).readLines().filter { it.isNotBlank() }
    check(reports.isNotEmpty()) { "Proxy tests emitted no status reports" }
    val states = reports.mapIndexed { index, report ->
        checkNotNull(ExperimentalAudioState.parse(report)) {
            "HAL status ${index + 1} rejected by Parts: $report"
        }
    }
    check(states.any { it.active == ExperimentalAudioMode.OFF })
    check(states.any { it.active == ExperimentalAudioMode.STEREO })
    check(states.any { it.active == ExperimentalAudioMode.DUAL_MONO })
    check(states.any { !it.available })
    check(states.any { it.outputActive == ExperimentalAudioOutput.UPPER })
    check(states.any { it.outputActive == ExperimentalAudioOutput.LOWER })
    println("HAL / Parts wire contract: ${reports.size} real reports PASS")
}
