// SPDX-License-Identifier: Apache-2.0
package org.lineageos.settings.tetris.audio

import android.media.AudioSystem
import android.os.SystemProperties
import java.io.IOException
import org.lineageos.settings.tetris.consent.PartsControlGate
import org.lineageos.settings.tetris.thermal.ThermalController
import org.lineageos.settings.tetris.network.TcpControlSession

private fun settingsReply(values: Map<String, String> = emptyMap()): String = linkedMapOf(
    "version" to "1", "available" to "1", "stereo_available" to "1",
    "requested" to "stereo", "active" to "off", "reason" to "idle",
    "gain_available" to "1", "gain_db" to "2",
    "software_gain_available" to "1", "software_gain_db" to "0",
    "sync_trim_available" to "1", "sync_trim_ms" to "0",
    "output_available" to "1", "output" to "both", "output_active" to "both",
).apply { putAll(values) }.entries.joinToString(";") { "tetris_audio_${it.key}=${it.value}" }

private data class Setting(
    val name: String, val field: String, val capability: String,
    val previous: String, val desired: String, val choices: List<String>,
    val apply: (ExperimentalAudioControl, String) -> ExperimentalAudioResult,
) {
    val property get() = "persist.sys.tetris.audio_$name"
    val parameter get() = "tetris_audio_$name"
}
private val settings = listOf(
    Setting("software_gain", "software_gain_db", "software_gain_available", "2", "4", (0..6).map(Int::toString)) { c, v -> c.applySoftwareGain(v.toInt()) },
    Setting("sync_trim", "sync_trim_ms", "sync_trim_available", "-3", "5", (-10..10).map(Int::toString)) { c, v -> c.applySyncTrim(v.toInt()) },
    Setting("output", "output", "output_available", "both", "upper", listOf("both", "upper", "lower")) { c, v -> c.applyOutput(checkNotNull(ExperimentalAudioOutput.fromWire(v))) },
)

private class Fixture(val setting: Setting) {
    val live = mutableMapOf(setting.field to setting.previous)
    val control = ExperimentalAudioRepository.control
    init {
        AudioSystem.reset()
        SystemProperties.reset()
        SystemProperties.values[setting.property] = setting.previous
        AudioSystem.readAction = { settingsReply(live) }
        AudioSystem.writeAction = ::accept
        control.read()
    }
    fun accept(request: String): Int {
        check(request.startsWith("${setting.parameter}=")) { "Unexpected write: $request" }
        val value = request.substringAfter('=')
        check(SystemProperties.get(setting.property) == value) { "Property must authorize before HAL write" }
        live[setting.field] = value
        return 0
    }
    fun apply(value: String = setting.desired) = setting.apply(control, value)
    fun unchanged() {
        check(SystemProperties.get(setting.property) == setting.previous)
        check(live[setting.field] == setting.previous)
    }
}

fun main(args: Array<String>) {
    if (args.singleOrNull() == "restart") {
        SystemProperties.values["persist.sys.tetris.audio_software_gain"] = "5"
        SystemProperties.values["persist.sys.tetris.audio_sync_trim"] = "-7"
        SystemProperties.values["persist.sys.tetris.audio_output"] = "upper"
        AudioSystem.readAction = { settingsReply(mapOf("requested" to "off", "software_gain_db" to "5",
            "sync_trim_ms" to "-7", "output" to "upper")) }
        repeat(3) {
            val state = ExperimentalAudioRepository.control.read()
            check(state.softwareGainDb == 5 && state.syncTrimMs == -7 && state.output == ExperimentalAudioOutput.UPPER)
            check(state.requested == ExperimentalAudioMode.OFF && state.active == ExperimentalAudioMode.OFF)
        }
        check(SystemProperties.writes.isEmpty() && AudioSystem.writes.isEmpty())
        println("PASS: restart preserves stored advanced preferences while OFF, without replay")
        return
    }
    settings.forEach { setting ->
        val fixture = Fixture(setting)
        PartsControlGate.allowed = false
        check(!fixture.apply().accepted)
        fixture.unchanged()
        check(SystemProperties.writes.isEmpty() && AudioSystem.writes.isEmpty())
    }
    // A stale native consent property cannot authorize a queued thermal callback.
    SystemProperties.reset()
    SystemProperties.values["sys.tetrisparts.consent"] = "1"
    PartsControlGate.allowed = false
    ThermalController.request(true)
    check(SystemProperties.writes.isEmpty())
    ThermalController.request(false)
    check(SystemProperties.get("sys.tetrisparts.thermal_disabled") == "0")
    // Pending network persistence must be rejected even before init clears readiness.
    SystemProperties.reset()
    SystemProperties.values["sys.tetrisparts.consent"] = "1"
    SystemProperties.values["sys.tetrisparts.tcp_ready"] = "1"
    PartsControlGate.allowed = false
    val tcp = TcpControlSession.manager
    val state = tcp.readState()
    check(!state.canApply)
    check(!tcp.setApplyOnBoot(true, state).successful)
    check(SystemProperties.writes.isEmpty())
    check(tcp.setApplyOnBoot(false, state).successful)
    check(SystemProperties.get("persist.sys.tetrisparts.tcp_congestion") == "off")
    println("PASS: queued audio, thermal and network mutations remain disabled after revocation")
    val old = settingsReply().split(';').filterNot { entry ->
        listOf("software_gain", "sync_trim", "output").any { entry.startsWith("tetris_audio_$it") }
    }.joinToString(";")
    val oldState = checkNotNull(ExperimentalAudioState.parse(old))
    check(!oldState.softwareGainAvailable && !oldState.syncTrimAvailable && !oldState.outputAvailable)
    check(oldState.softwareGainDb == 0 && oldState.syncTrimMs == 0 && oldState.output == ExperimentalAudioOutput.BOTH)
    for ((field, invalid) in listOf("software_gain_db" to listOf("-1", "7", "15", "bad", "2147483648"),
            "sync_trim_ms" to listOf("-11", "11", "1.5", "bad", "2147483648"),
            "output" to listOf("left", "right", "UPPER", "", "upper=1"),
            "output_active" to listOf("left", "invalid"))) {
        for (value in invalid) check(ExperimentalAudioState.parse(settingsReply(mapOf(field to value))) == null)
    }
    for (setting in settings) {
        val withoutValue = settingsReply().split(';').filterNot { it.startsWith("tetris_audio_${setting.field}=") }.joinToString(";")
        val withoutFlag = settingsReply().split(';').filterNot { it.startsWith("tetris_audio_${setting.capability}=") }.joinToString(";")
        check(ExperimentalAudioState.parse(withoutValue) == null && ExperimentalAudioState.parse(withoutFlag) == null)
        check(ExperimentalAudioState.parse(settingsReply(mapOf(setting.capability to "true"))) == null)
    }
    check(ExperimentalAudioState.parse(settingsReply(mapOf("stereo_available" to "0"))) == null)
    val liveUpper = checkNotNull(ExperimentalAudioState.parse(settingsReply(mapOf("output" to "lower", "output_active" to "upper"))))
    check(liveUpper.output == ExperimentalAudioOutput.LOWER && liveUpper.outputActive == ExperimentalAudioOutput.UPPER)
    val legacyActive = settingsReply(mapOf("output" to "upper")).split(';').filterNot { it.startsWith("tetris_audio_output_active=") }.joinToString(";")
    check(ExperimentalAudioState.parse(legacyActive)?.outputActive == ExperimentalAudioOutput.UPPER)
    println("PASS: optional fields support old HALs; malformed, incomplete and unsupported values are rejected")

    for (setting in settings) {
        var f = Fixture(setting)
        for (choice in setting.choices) {
            val result = f.apply(choice)
            check(result.accepted && result.state.active == ExperimentalAudioMode.OFF)
            check(f.live[setting.field] == choice && SystemProperties.get(setting.property) == choice)
            check(result.state.gainDb == 2) { "Codec gain must remain separate" }
        }
        check(SystemProperties.writes.all { it.first == setting.property })
        println("PASS: ${setting.name} persists every supported value with readback, including idle playback")

        f = Fixture(setting)
        f.live[setting.capability] = "0"
        check(!f.apply().accepted && AudioSystem.writes.isEmpty() && SystemProperties.writes.isEmpty())
        f = Fixture(setting)
        SystemProperties.setFailure = { _, _ -> SecurityException("Denied") }
        check(!f.apply().accepted && AudioSystem.writes.isEmpty())
        f.unchanged()
        println("PASS: ${setting.name} capability and authorization are required before writing")

        f = Fixture(setting)
        AudioSystem.writeAction = { request -> f.accept(request); if (request.endsWith("=${setting.desired}")) -1 else 0 }
        check(!f.apply().accepted)
        f.unchanged()
        check(AudioSystem.writes == listOf("${setting.parameter}=${setting.desired}", "${setting.parameter}=${setting.previous}"))

        f = Fixture(setting)
        AudioSystem.writeAction = { 0 }
        check(!f.apply().accepted)
        f.unchanged()

        for (throws in listOf(false, true)) {
            f = Fixture(setting)
            var loseRead = false
            AudioSystem.readAction = {
                if (loseRead) {
                    loseRead = false
                    if (throws) throw IOException("Lost acknowledgement")
                    null
                } else settingsReply(f.live)
            }
            AudioSystem.writeAction = { request ->
                f.accept(request)
                if (request.endsWith("=${setting.desired}")) loseRead = true
                0
            }
            check(!f.apply().accepted)
            f.unchanged()
        }
        f = Fixture(setting)
        AudioSystem.writeAction = { throw IOException("HAL died") }
        check(!f.apply().accepted)
        f.unchanged()
        println("PASS: ${setting.name} refusal, no-op and lost acknowledgements restore saved and live values")

        f = Fixture(setting)
        val newer = setting.choices.first { it != setting.desired && it != setting.previous }
        AudioSystem.writeAction = {
            SystemProperties.values[setting.property] = newer
            f.live[setting.field] = newer
            -1
        }
        check(!f.apply().accepted)
        check(SystemProperties.get(setting.property) == newer && f.live[setting.field] == newer)
        check(SystemProperties.writes == listOf(setting.property to setting.desired))
        check(AudioSystem.writes.size == 1)
        println("PASS: ${setting.name} rollback preserves a newer authorization")

        f = Fixture(setting)
        repeat(3) { f.control.read() }
        check(AudioSystem.writes.isEmpty() && SystemProperties.writes.isEmpty())
        AudioSystem.readAction = { null }
        val unavailable = f.control.read()
        check(!unavailable.softwareGainAvailable && !unavailable.syncTrimAvailable && !unavailable.outputAvailable)
        check(unavailable.active == ExperimentalAudioMode.OFF)
    }
    for (setting in settings) {
        val f = Fixture(setting)
        f.live["requested"] = "off"
        check(!f.apply().accepted && AudioSystem.writes.isEmpty())
    }
    for (setting in settings.take(2)) {
        val f = Fixture(setting)
        f.live["requested"] = "dual_mono"
        check(!f.apply().accepted && AudioSystem.writes.isEmpty())
    }
    var f = Fixture(settings[0])
    f.live["requested"] = "dual_mono"
    f.live["output"] = "upper"
    check(f.apply().accepted)
    f = Fixture(settings[0])
    f.live["output"] = "lower"
    check(!f.apply().accepted && AudioSystem.writes.isEmpty())
    f = Fixture(settings[1])
    f.live["output"] = "upper"
    check(!f.apply().accepted && AudioSystem.writes.isEmpty())
    f = Fixture(settings[0])
    check(!f.control.applySoftwareGain(-1).accepted && !f.control.applySoftwareGain(7).accepted)
    check(AudioSystem.writes.isEmpty())
    f = Fixture(settings[1])
    check(!f.control.applySyncTrim(-11).accepted && !f.control.applySyncTrim(11).accepted)
    check(AudioSystem.writes.isEmpty())
    println("PASS: mode/output gates and boundaries prevent ineffective or out-of-range controls")
}
