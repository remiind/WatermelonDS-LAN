package me.magnum.melonds.lan

import android.content.Context
import android.net.wifi.WifiManager
import android.os.Build
import android.util.Log
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.delay
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.flow.update
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext

/**
 * Process-wide owner of the LAN multiplayer session.
 *
 * melonDS keeps one global MP interface, so the session outlives the lobby
 * screen: you host or join in the lobby, go back to the ROM list and start the
 * same game on every device. While no game is running, a background poller
 * keeps discovery beacons and ENet connections alive; once a game runs, the
 * emulator thread takes over (the native side arbitrates between the two).
 */
object LanSession {

    private const val TAG = "LanSession"
    private const val POLL_INTERVAL_MS = 16L
    private const val LIST_REFRESH_MS = 500L

    enum class Mode { IDLE, BROWSING, HOSTING, CLIENT }

    data class State(
        val mode: Mode = Mode.IDLE,
        val busy: Boolean = false,
        val error: String? = null,
        val discovered: List<MelonLan.DiscoveredSession> = emptyList(),
        val players: List<MelonLan.Player> = emptyList(),
        val maxPlayers: Int = 0,
    )

    private val scope = CoroutineScope(SupervisorJob() + Dispatchers.Default)
    private val _state = MutableStateFlow(State())
    val state: StateFlow<State> = _state.asStateFlow()

    private var pollJob: Job? = null
    private var multicastLock: WifiManager.MulticastLock? = null
    private var wifiLock: WifiManager.WifiLock? = null

    val hasSession: Boolean
        get() = _state.value.mode == Mode.HOSTING || _state.value.mode == Mode.CLIENT

    /** Starts listening for hosted sessions on the local network. */
    fun startBrowsing(context: Context) {
        if (_state.value.mode != Mode.IDLE) return
        launchAction(context) {
            if (!MelonLan.enable()) return@launchAction fail(ERROR_GAME_RUNNING)
            if (!MelonLan.startDiscovery()) {
                MelonLan.disable()
                return@launchAction fail(ERROR_DISCOVERY)
            }
            _state.update { it.copy(mode = Mode.BROWSING, error = null) }
        }
    }

    fun host(context: Context, playerName: String, maxPlayers: Int) {
        launchAction(context) {
            resetToIdleNative()
            if (!MelonLan.enable()) return@launchAction fail(ERROR_GAME_RUNNING)
            if (!MelonLan.startHost(playerName.ifBlank { "Player" }, maxPlayers.coerceIn(2, 16))) {
                MelonLan.disable()
                return@launchAction fail(ERROR_HOST)
            }
            _state.update { it.copy(mode = Mode.HOSTING, error = null) }
        }
    }

    fun join(context: Context, playerName: String, hostAddress: String) {
        launchAction(context) {
            if (!MelonLan.enable()) return@launchAction fail(ERROR_GAME_RUNNING)
            // StartClient blocks while ENet connects (up to ~5 s).
            val connected = MelonLan.startClient(playerName.ifBlank { "Player" }, hostAddress.trim())
            if (!connected) {
                // Go back to browsing so the user can pick another session.
                MelonLan.startDiscovery()
                _state.update { it.copy(mode = Mode.BROWSING) }
                return@launchAction fail(ERROR_JOIN)
            }
            _state.update { it.copy(mode = Mode.CLIENT, error = null) }
        }
    }

    /** Leaves the session (or stops browsing) and returns to single player. */
    fun leave() {
        scope.launch {
            if (MelonLan.isGameRunning()) {
                fail(ERROR_GAME_RUNNING)
                return@launch
            }
            resetToIdleNative()
            stopPolling()
            _state.value = State()
        }
    }

    fun clearError() {
        _state.update { it.copy(error = null) }
    }

    private fun resetToIdleNative() {
        if (MelonLan.isEnabled()) {
            MelonLan.endSession()
            MelonLan.endDiscovery()
            MelonLan.disable()
        }
    }

    private fun launchAction(context: Context, action: suspend () -> Unit) {
        if (_state.value.busy) return
        val appContext = context.applicationContext
        _state.update { it.copy(busy = true, error = null) }
        scope.launch {
            try {
                acquireMulticastLock(appContext)
                startPolling()
                action()
            } catch (e: Throwable) {
                Log.e(TAG, "LAN action failed", e)
                fail(e.message ?: e.javaClass.simpleName)
            } finally {
                _state.update { it.copy(busy = false) }
                if (_state.value.mode == Mode.IDLE) {
                    stopPolling()
                }
            }
        }
    }

    private fun fail(error: String) {
        _state.update { it.copy(error = error) }
    }

    private fun startPolling() {
        if (pollJob?.isActive == true) return
        pollJob = scope.launch {
            var lastListRefresh = 0L
            while (isActive) {
                MelonLan.poll()
                val now = System.currentTimeMillis()
                if (now - lastListRefresh >= LIST_REFRESH_MS) {
                    lastListRefresh = now
                    refreshLists()
                }
                delay(POLL_INTERVAL_MS)
            }
        }
    }

    private fun refreshLists() {
        if (!MelonLan.isEnabled()) return
        val mode = _state.value.mode
        val discovered = if (mode == Mode.BROWSING) MelonLan.discoveredSessions() else emptyList()
        val players = if (mode == Mode.HOSTING || mode == Mode.CLIENT) MelonLan.players() else emptyList()
        val maxPlayers = MelonLan.maxPlayers()
        _state.update { it.copy(discovered = discovered, players = players, maxPlayers = maxPlayers) }
    }

    private suspend fun stopPolling() {
        pollJob?.cancel()
        pollJob = null
        withContext(Dispatchers.Main) { releaseMulticastLock() }
    }

    private fun acquireMulticastLock(context: Context) {
        if (multicastLock?.isHeld == true) return
        val wifiManager = context.getSystemService(Context.WIFI_SERVICE) as? WifiManager ?: return
        // Some devices drop incoming UDP broadcasts (session discovery) without it.
        multicastLock = wifiManager.createMulticastLock("WatermelonDS-LAN").apply {
            setReferenceCounted(false)
            try {
                acquire()
            } catch (e: SecurityException) {
                Log.w(TAG, "Could not acquire multicast lock", e)
            }
        }

        // Wi-Fi power saving buffers packets for 100+ ms, which is far longer than
        // the DS wireless timing tolerates. Keep the radio in low-latency mode
        // while a LAN session exists.
        if (wifiLock?.isHeld != true) {
            val mode = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.Q) {
                WifiManager.WIFI_MODE_FULL_LOW_LATENCY
            } else {
                @Suppress("DEPRECATION")
                WifiManager.WIFI_MODE_FULL_HIGH_PERF
            }
            wifiLock = wifiManager.createWifiLock(mode, "WatermelonDS-LAN").apply {
                setReferenceCounted(false)
                try {
                    acquire()
                } catch (e: SecurityException) {
                    Log.w(TAG, "Could not acquire Wi-Fi lock", e)
                }
            }
        }
    }

    private fun releaseMulticastLock() {
        multicastLock?.let { if (it.isHeld) it.release() }
        multicastLock = null
        wifiLock?.let { if (it.isHeld) it.release() }
        wifiLock = null
    }

    // Stable keys, mapped to localized text in the UI.
    const val ERROR_GAME_RUNNING = "game_running"
    const val ERROR_DISCOVERY = "discovery"
    const val ERROR_HOST = "host"
    const val ERROR_JOIN = "join"
}
