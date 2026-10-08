package me.magnum.melonds.lan

import me.magnum.melonds.NativeCoreLoader

/**
 * JNI bindings for melonDS' LAN multiplayer (see app/src/main/cpp/MelonLan.cpp).
 *
 * Use [LanSession] instead of calling these directly: it owns the lobby poller,
 * the Wi-Fi multicast lock and the state exposed to the UI.
 */
object MelonLan {

    init {
        NativeCoreLoader.load()
    }

    data class DiscoveredSession(
        val address: String,
        val name: String,
        val numPlayers: Int,
        val maxPlayers: Int,
        val isPlaying: Boolean,
    )

    enum class PlayerStatus {
        NONE,
        CLIENT,
        HOST,
        CONNECTING,
        DISCONNECTED;

        companion object {
            fun fromNative(value: Int): PlayerStatus = PlayerStatus.entries.getOrElse(value) { NONE }
        }
    }

    data class Player(
        val id: Int,
        val name: String,
        val status: PlayerStatus,
        val address: String,
        val isLocal: Boolean,
        val pingMs: Int,
    )

    fun enable(): Boolean = nativeEnable()
    fun disable(): Boolean = nativeDisable()
    fun isEnabled(): Boolean = nativeIsEnabled()
    fun isGameRunning(): Boolean = nativeIsGameRunning()
    fun startHost(playerName: String, maxPlayers: Int): Boolean = nativeStartHost(playerName, maxPlayers)
    fun startDiscovery(): Boolean = nativeStartDiscovery()
    fun endDiscovery() = nativeEndDiscovery()

    /** Blocks for up to ~5 seconds. Never call on the main thread. */
    fun startClient(playerName: String, host: String): Boolean = nativeStartClient(playerName, host)
    fun endSession() = nativeEndSession()
    fun poll() = nativePoll()
    fun maxPlayers(): Int = nativeGetMaxPlayers()

    fun discoveredSessions(): List<DiscoveredSession> {
        return nativeGetDiscoveryList().orEmpty().mapNotNull { entry ->
            val fields = entry.split('\t')
            if (fields.size < 5) return@mapNotNull null
            DiscoveredSession(
                address = fields[0],
                name = fields[1],
                numPlayers = fields[2].toIntOrNull() ?: 0,
                maxPlayers = fields[3].toIntOrNull() ?: 0,
                isPlaying = fields[4] == "1",
            )
        }
    }

    fun players(): List<Player> {
        return nativeGetPlayerList().orEmpty().mapNotNull { entry ->
            val fields = entry.split('\t')
            if (fields.size < 6) return@mapNotNull null
            Player(
                id = fields[0].toIntOrNull() ?: return@mapNotNull null,
                name = fields[1],
                status = PlayerStatus.fromNative(fields[2].toIntOrNull() ?: 0),
                address = fields[3],
                isLocal = fields[4] == "1",
                pingMs = fields[5].toIntOrNull() ?: 0,
            )
        }
    }

    private external fun nativeEnable(): Boolean
    private external fun nativeDisable(): Boolean
    private external fun nativeIsEnabled(): Boolean
    private external fun nativeIsGameRunning(): Boolean
    private external fun nativeStartHost(playerName: String, maxPlayers: Int): Boolean
    private external fun nativeStartDiscovery(): Boolean
    private external fun nativeEndDiscovery()
    private external fun nativeStartClient(playerName: String, host: String): Boolean
    private external fun nativeEndSession()
    private external fun nativePoll()
    private external fun nativeGetDiscoveryList(): Array<String>?
    private external fun nativeGetPlayerList(): Array<String>?
    private external fun nativeGetMaxPlayers(): Int
}
