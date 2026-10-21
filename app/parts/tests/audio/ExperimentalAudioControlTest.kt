/* SPDX-FileCopyrightText: 2026 The LineageOS Project
 * SPDX-License-Identifier: Apache-2.0
 */
package org.lineageos.settings.tetris.audio

import java.io.IOException

private fun response(vararg overrides: Pair<String, String>): String = linkedMapOf(
    "version" to "1",
    "available" to "1",
    "stereo_available" to "1",
    "requested" to "off",
    "active" to "off",
    "reason" to "idle",
).apply { putAll(overrides) }.entries.joinToString(";") { (key, value) ->
    "tetris_audio_$key=$value"
}

private class Transport(initial: String? = response()) : ExperimentalAudioTransport {
    var parameters: String? = initial
    var readFails = false
    val writes = mutableListOf<ExperimentalAudioMode>()
    var action: (ExperimentalAudioMode) -> Int = { mode ->
        parameters = response("requested" to mode.wireValue)
        0
    }

    override fun read(): String? {
        if (readFails) throw IOException("Audio HAL disconnected")
        return parameters
    }

    override fun write(mode: ExperimentalAudioMode): Int {
        writes += mode
        return action(mode)
    }
}

fun main() {
    var cases = 0
    fun test(name: String, block: () -> Unit) {
        block()
        cases++
        println("PASS: $name")
    }

    test("versioned response reports requested mode separately from an idle route") {
        val state = checkNotNull(ExperimentalAudioState.parse(response("requested" to "stereo")))
        check(state.available && state.stereoAvailable)
        check(state.requested == ExperimentalAudioMode.STEREO)
        check(state.active == ExperimentalAudioMode.OFF && state.reason == "idle")
    }
    test("empty, unversioned and unknown protocol replies remain unavailable") {
        listOf(null, "", " \n\t", "supported=true", "tetris_audio_available=1",
            response("version" to "0"), response("version" to "2"), response("version" to ""),
            response() + ";" + "x".repeat(4096),
        ).forEach { check(ExperimentalAudioState.parse(it) == null) { "Accepted invalid reply: $it" } }
    }
    test("unrelated HAL parameters and extension fields do not replace required fields") {
        val parsed = ExperimentalAudioState.parse("unrelated=1;${response()};tetris_audio_extension=extra;")
        check(parsed == ExperimentalAudioState.parse(response()))
        check(ExperimentalAudioState.parse("version=1;available=1;requested=off;active=off;reason=idle") == null)
    }
    test("duplicate owned keys cannot ambiguously acknowledge a request") {
        listOf("available=1", "available=0", "requested=off", "requested=stereo", "version=1").forEach {
            check(ExperimentalAudioState.parse("${response()};tetris_audio_$it") == null)
        }
        check(ExperimentalAudioState.parse("${response()}; tetris_audio_active =off") == null)
    }
    test("boolean capability values use only zero and one") {
        listOf("", "true", "false", "yes", "-1", "2", "01").forEach { value ->
            check(ExperimentalAudioState.parse(response("available" to value)) == null)
            check(ExperimentalAudioState.parse(response("stereo_available" to value)) == null)
        }
        check(ExperimentalAudioState.parse(response("available" to "0", "stereo_available" to "0")) != null)
        check(ExperimentalAudioState.parse(response("available" to "0")) == null)
    }
    test("unknown or malformed modes and reasons are rejected") {
        listOf("", "mono", "DUAL_MONO", "left", "stereo=1", "off\nstereo").forEach { value ->
            check(ExperimentalAudioState.parse(response("requested" to value)) == null)
            check(ExperimentalAudioState.parse(response("active" to value)) == null)
        }
        listOf("", "Call", "call-mode", "error 1", "x".repeat(65)).forEach { reason ->
            check(ExperimentalAudioState.parse(response("reason" to reason)) == null)
        }
        check(ExperimentalAudioState.parse(response().replace("tetris_audio_version=1", "malformed")) == null)
    }
    test("active output must match the request and available capabilities") {
        val modes = ExperimentalAudioMode.entries
        for (available in listOf(false, true)) {
            for (stereo in listOf(false, true)) {
                for (requested in modes) for (active in modes) {
                    val expected = !(stereo && !available) &&
                        (active == ExperimentalAudioMode.OFF || (available && active == requested)) &&
                        (active != ExperimentalAudioMode.STEREO || stereo)
                    val wire = response("available" to if (available) "1" else "0",
                        "stereo_available" to if (stereo) "1" else "0",
                        "requested" to requested.wireValue, "active" to active.wireValue)
                    check((ExperimentalAudioState.parse(wire) != null) == expected) { wire }
                }
            }
        }
    }
    test("initialization, reads and controller recreation never change the audio mode") {
        val transport = Transport(response("requested" to "dual_mono", "active" to "dual_mono"))
        val control = ExperimentalAudioControl(transport)
        repeat(3) { check(control.read().requested == ExperimentalAudioMode.DUAL_MONO) }
        check(ExperimentalAudioControl(transport).read().active == ExperimentalAudioMode.DUAL_MONO)
        check(transport.writes.isEmpty())
    }
    test("missing capability refuses both enable modes without writing") {
        listOf(null, response("available" to "0", "stereo_available" to "0")).forEach { reply ->
            val transport = Transport(reply)
            val control = ExperimentalAudioControl(transport)
            check(!control.apply(ExperimentalAudioMode.DUAL_MONO).accepted)
            check(!control.apply(ExperimentalAudioMode.STEREO).accepted)
            check(transport.writes.isEmpty())
        }
    }
    test("stereo capability is required only for stereo requests") {
        val transport = Transport(response("stereo_available" to "0"))
        val control = ExperimentalAudioControl(transport)
        check(!control.apply(ExperimentalAudioMode.STEREO).accepted)
        check(transport.writes.isEmpty())
        transport.action = { mode ->
            transport.parameters = response("stereo_available" to "0", "requested" to mode.wireValue)
            0
        }
        check(control.apply(ExperimentalAudioMode.DUAL_MONO).accepted)
        check(transport.writes == listOf(ExperimentalAudioMode.DUAL_MONO))
    }
    test("a service outage preserves the enabled preference but never claims an active route") {
        val transport = Transport(response("requested" to "stereo", "active" to "stereo"))
        val control = ExperimentalAudioControl(transport)
        check(control.read().active == ExperimentalAudioMode.STEREO)
        transport.readFails = true
        val unavailable = control.read()
        check(!unavailable.available && !unavailable.stereoAvailable)
        check(unavailable.requested == ExperimentalAudioMode.STEREO)
        check(unavailable.active == ExperimentalAudioMode.OFF && unavailable.reason == "unavailable")
        transport.action = { mode ->
            check(mode == ExperimentalAudioMode.OFF)
            transport.readFails = false
            transport.parameters = response("available" to "0", "stereo_available" to "0")
            0
        }
        check(control.apply(ExperimentalAudioMode.OFF).accepted)
        check(transport.writes == listOf(ExperimentalAudioMode.OFF))
    }
    test("explicit capability downgrade retains the option to disable the old request") {
        val transport = Transport(response("requested" to "stereo", "active" to "stereo"))
        val control = ExperimentalAudioControl(transport)
        control.read()
        transport.parameters = response("available" to "0", "stereo_available" to "0",
            "requested" to "stereo", "reason" to "unsupported")
        val downgraded = control.read()
        check(!downgraded.available && downgraded.requested == ExperimentalAudioMode.STEREO)
        check(downgraded.active == ExperimentalAudioMode.OFF)
        check(control.apply(ExperimentalAudioMode.OFF).accepted)
        check(transport.writes == listOf(ExperimentalAudioMode.OFF))
    }
    test("an unrelated HAL accepting an unknown write does not acknowledge it") {
        val transport = Transport(null).apply { action = { 0 } }
        val control = ExperimentalAudioControl(transport)
        val result = control.apply(ExperimentalAudioMode.OFF)
        check(!result.accepted && result.state.reason == "unavailable")
        check(transport.writes == listOf(ExperimentalAudioMode.OFF))
    }
    test("write success without matching readback is refused") {
        val transport = Transport().apply { action = { 0 } }
        val result = ExperimentalAudioControl(transport).apply(ExperimentalAudioMode.STEREO)
        check(!result.accepted && result.state.requested == ExperimentalAudioMode.OFF)
        check(transport.writes == listOf(ExperimentalAudioMode.STEREO))
    }
    test("readback disappearance after an accepted write is not success") {
        val transport = Transport().apply { action = { parameters = null; 0 } }
        val result = ExperimentalAudioControl(transport).apply(ExperimentalAudioMode.DUAL_MONO)
        check(!result.accepted && !result.state.available)
        check(result.state.active == ExperimentalAudioMode.OFF)
    }
    test("a failed or throwing write cannot be reported as accepted") {
        for (failsWithException in listOf(false, true)) {
            val transport = Transport().apply {
                action = { mode ->
                    // Even a concurrent matching state change cannot turn a failed request into success.
                    parameters = response("requested" to mode.wireValue)
                    if (failsWithException) throw SecurityException("Permission denied")
                    -1
                }
            }
            val result = ExperimentalAudioControl(transport).apply(ExperimentalAudioMode.DUAL_MONO)
            check(!result.accepted)
            check(result.state.requested == ExperimentalAudioMode.DUAL_MONO)
            check(transport.writes == listOf(ExperimentalAudioMode.DUAL_MONO))
        }
    }
    test("successful readback acknowledges the preference without claiming immediate playback") {
        val transport = Transport()
        val control = ExperimentalAudioControl(transport)
        for (mode in listOf(ExperimentalAudioMode.DUAL_MONO, ExperimentalAudioMode.STEREO,
                ExperimentalAudioMode.OFF)) {
            val result = control.apply(mode)
            check(result.accepted && result.state.requested == mode)
            check(result.state.active == ExperimentalAudioMode.OFF)
        }
        check(transport.writes == listOf(ExperimentalAudioMode.DUAL_MONO,
            ExperimentalAudioMode.STEREO, ExperimentalAudioMode.OFF))
    }
    test("malformed refresh clears active state while preserving an OFF escape action") {
        val transport = Transport(response("requested" to "dual_mono", "active" to "dual_mono"))
        val control = ExperimentalAudioControl(transport)
        control.read()
        transport.parameters = response("active" to "stereo", "requested" to "dual_mono")
        check(control.read() == ExperimentalAudioState(requested = ExperimentalAudioMode.DUAL_MONO))
        check(control.apply(ExperimentalAudioMode.OFF).accepted)
        check(transport.writes == listOf(ExperimentalAudioMode.OFF))
    }
    println("$cases experimental audio control tests passed")
}
