package me.magnum.melonds.domain.model.emulator

import me.magnum.melonds.domain.model.VideoRenderer

sealed class EmulatorEvent {
    data class RumbleStart(val duration: Int) : EmulatorEvent()
    data object RumbleStop : EmulatorEvent()
    /** WatermelonDS-LAN: shiny wild Pokémon found, emulation paused by the native side. */
    data object PokeCounterShiny : EmulatorEvent()
    /** WatermelonDS-LAN: egg hunt state changed (code, see MelonInstance.cpp). */
    data class EggHunt(val code: Int) : EmulatorEvent()
    data class RendererInitFailed(val renderer: VideoRenderer) : EmulatorEvent()
    data class VulkanCompileProgress(
        val stageId: Int,
        val current: Int,
        val total: Int,
    ) : EmulatorEvent()
    data class Stop(val reason: Reason) : EmulatorEvent() {
        enum class Reason {
            GBAModeNotSupported,
            BadExceptionRegion,
            PowerOff,
        }
    }
}
