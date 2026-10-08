package me.magnum.melonds.ui.lan

import android.content.Context
import android.content.Intent
import android.graphics.Color
import android.os.Bundle
import androidx.activity.SystemBarStyle
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.appcompat.app.AppCompatActivity
import androidx.compose.foundation.background
import androidx.compose.foundation.clickable
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.WindowInsets
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.safeDrawing
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.statusBarsPadding
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.rememberScrollState
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.foundation.text.KeyboardOptions
import androidx.compose.foundation.verticalScroll
import androidx.compose.material.Button
import androidx.compose.material.CircularProgressIndicator
import androidx.compose.material.Divider
import androidx.compose.material.Icon
import androidx.compose.material.IconButton
import androidx.compose.material.MaterialTheme
import androidx.compose.material.OutlinedButton
import androidx.compose.material.OutlinedTextField
import androidx.compose.material.Scaffold
import androidx.compose.material.Text
import androidx.compose.material.TopAppBar
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.automirrored.filled.ArrowBack
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.input.KeyboardType
import androidx.compose.ui.unit.dp
import androidx.core.content.edit
import me.magnum.melonds.R
import me.magnum.melonds.lan.LanSession
import me.magnum.melonds.lan.MelonLan
import me.magnum.melonds.ui.theme.MelonTheme
import me.magnum.melonds.ui.theme.watermelon

/**
 * Lobby for LAN multiplayer. Host or join a session here, then go back to the
 * ROM list and start the same game on every device. The session stays active
 * after leaving this screen until "Leave session" is pressed.
 */
class LanMultiplayerActivity : AppCompatActivity() {

    companion object {
        private const val PREFS = "lan_multiplayer"
        private const val KEY_PLAYER_NAME = "player_name"

        fun getIntent(context: Context): Intent = Intent(context, LanMultiplayerActivity::class.java)
    }

    override fun onCreate(savedInstanceState: Bundle?) {
        enableEdgeToEdge(statusBarStyle = SystemBarStyle.dark(Color.TRANSPARENT))
        super.onCreate(savedInstanceState)

        val prefs = getSharedPreferences(PREFS, MODE_PRIVATE)
        val initialName = prefs.getString(KEY_PLAYER_NAME, null) ?: "Player"

        setContent {
            MelonTheme {
                LanScreen(
                    initialPlayerName = initialName,
                    onPlayerNameChanged = { name -> prefs.edit { putString(KEY_PLAYER_NAME, name) } },
                    onBack = { finish() },
                )
            }
        }
    }

    override fun onResume() {
        super.onResume()
        // Start listening for sessions as soon as the lobby is visible.
        if (LanSession.state.value.mode == LanSession.Mode.IDLE) {
            LanSession.startBrowsing(this)
        }
    }

    override fun onDestroy() {
        // Stop browsing when the lobby closes, but keep an established session.
        if (isFinishing && LanSession.state.value.mode == LanSession.Mode.BROWSING) {
            LanSession.leave()
        }
        super.onDestroy()
    }
}

@Composable
private fun LanScreen(
    initialPlayerName: String,
    onPlayerNameChanged: (String) -> Unit,
    onBack: () -> Unit,
) {
    val context = androidx.compose.ui.platform.LocalContext.current
    val state by LanSession.state.collectAsState()
    var playerName by remember { mutableStateOf(initialPlayerName) }
    var maxPlayers by remember { mutableIntStateOf(2) }
    var manualAddress by remember { mutableStateOf("") }
    val colors = watermelon

    Scaffold(
        topBar = {
            Box(Modifier.background(MaterialTheme.colors.background).statusBarsPadding()) {
                TopAppBar(
                    title = { Text(stringResource(R.string.lan_title)) },
                    navigationIcon = {
                        IconButton(onClick = onBack) {
                            Icon(Icons.AutoMirrored.Filled.ArrowBack, contentDescription = null)
                        }
                    },
                    backgroundColor = MaterialTheme.colors.background,
                    contentColor = MaterialTheme.colors.onBackground,
                    elevation = 0.dp,
                )
            }
        },
        backgroundColor = MaterialTheme.colors.background,
        contentWindowInsets = WindowInsets.safeDrawing,
    ) { padding ->
        Column(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding)
                .verticalScroll(rememberScrollState())
                .padding(16.dp),
            verticalArrangement = Arrangement.spacedBy(12.dp),
        ) {
            state.error?.let { error ->
                ErrorBanner(text = errorText(error), onDismiss = LanSession::clearError)
            }

            val inSession = state.mode == LanSession.Mode.HOSTING || state.mode == LanSession.Mode.CLIENT

            OutlinedTextField(
                value = playerName,
                onValueChange = {
                    playerName = it.take(31)
                    onPlayerNameChanged(playerName)
                },
                label = { Text(stringResource(R.string.lan_player_name)) },
                singleLine = true,
                enabled = !inSession && !state.busy,
                modifier = Modifier.fillMaxWidth(),
            )

            if (state.busy) {
                Row(verticalAlignment = Alignment.CenterVertically) {
                    CircularProgressIndicator(modifier = Modifier.size(20.dp), strokeWidth = 2.dp)
                    Spacer(Modifier.width(12.dp))
                    Text(stringResource(R.string.lan_working), color = colors.text2)
                }
            }

            if (inSession) {
                SessionSection(state = state)
                Text(stringResource(R.string.lan_session_hint), color = colors.text2)
                Button(
                    onClick = { LanSession.leave() },
                    enabled = !state.busy,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(stringResource(R.string.lan_leave))
                }
            } else {
                SectionTitle(stringResource(R.string.lan_host_title))
                Row(verticalAlignment = Alignment.CenterVertically) {
                    Text(stringResource(R.string.lan_max_players), modifier = Modifier.weight(1f))
                    OutlinedButton(onClick = { if (maxPlayers > 2) maxPlayers-- }, enabled = !state.busy) { Text("−") }
                    Text("$maxPlayers", modifier = Modifier.padding(horizontal = 16.dp), fontWeight = FontWeight.Bold)
                    OutlinedButton(onClick = { if (maxPlayers < 16) maxPlayers++ }, enabled = !state.busy) { Text("+") }
                }
                Button(
                    onClick = { LanSession.host(context, playerName, maxPlayers) },
                    enabled = !state.busy,
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(stringResource(R.string.lan_host))
                }

                Divider(Modifier.padding(vertical = 4.dp))

                SectionTitle(stringResource(R.string.lan_join_title))
                if (state.discovered.isEmpty()) {
                    Text(stringResource(R.string.lan_searching), color = colors.text2)
                } else {
                    state.discovered.forEach { session ->
                        DiscoveredRow(
                            session = session,
                            enabled = !state.busy,
                            onClick = { LanSession.join(context, playerName, session.address) },
                        )
                    }
                }

                OutlinedTextField(
                    value = manualAddress,
                    onValueChange = { manualAddress = it.trim() },
                    label = { Text(stringResource(R.string.lan_manual_address)) },
                    singleLine = true,
                    keyboardOptions = KeyboardOptions(keyboardType = KeyboardType.Uri),
                    enabled = !state.busy,
                    modifier = Modifier.fillMaxWidth(),
                )
                OutlinedButton(
                    onClick = { LanSession.join(context, playerName, manualAddress) },
                    enabled = !state.busy && manualAddress.isNotBlank(),
                    modifier = Modifier.fillMaxWidth(),
                ) {
                    Text(stringResource(R.string.lan_join_address))
                }
            }

            Spacer(Modifier.height(8.dp))
            Text(stringResource(R.string.lan_footer), color = colors.text3)
        }
    }
}

@Composable
private fun SessionSection(state: LanSession.State) {
    val colors = watermelon
    val title = if (state.mode == LanSession.Mode.HOSTING) R.string.lan_hosting else R.string.lan_connected
    SectionTitle(stringResource(title))

    val maxPlayers = state.maxPlayers.takeIf { it > 0 } ?: state.players.size
    state.players.sortedBy { it.id }.forEach { player ->
        Row(
            modifier = Modifier
                .fillMaxWidth()
                .background(colors.surface, RoundedCornerShape(8.dp))
                .padding(12.dp),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text("${player.id + 1}/$maxPlayers", color = colors.text2, modifier = Modifier.width(48.dp))
            Column(Modifier.weight(1f)) {
                Text(player.name, fontWeight = FontWeight.Bold)
                Text(playerStatusText(player), color = colors.text2)
            }
            if (!player.isLocal && (player.status == MelonLan.PlayerStatus.CLIENT || player.status == MelonLan.PlayerStatus.HOST)) {
                Text("${player.pingMs} ms", color = colors.text2)
            }
        }
    }
}

@Composable
private fun DiscoveredRow(session: MelonLan.DiscoveredSession, enabled: Boolean, onClick: () -> Unit) {
    val colors = watermelon
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(colors.surface, RoundedCornerShape(8.dp))
            .clickable(enabled = enabled && !session.isPlaying, onClick = onClick)
            .padding(12.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Column(Modifier.weight(1f)) {
            Text(session.name.ifBlank { session.address }, fontWeight = FontWeight.Bold)
            Text(session.address, color = colors.text2)
        }
        Column(horizontalAlignment = Alignment.End) {
            Text("${session.numPlayers}/${session.maxPlayers}")
            Text(
                stringResource(if (session.isPlaying) R.string.lan_status_playing else R.string.lan_status_open),
                color = colors.text2,
            )
        }
    }
}

@Composable
private fun SectionTitle(text: String) {
    Text(text, style = MaterialTheme.typography.h6)
}

@Composable
private fun ErrorBanner(text: String, onDismiss: () -> Unit) {
    val colors = watermelon
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .background(colors.redGlow, RoundedCornerShape(8.dp))
            .clickable(onClick = onDismiss)
            .padding(12.dp),
    ) {
        Text(text, color = colors.text)
    }
}

@Composable
private fun errorText(error: String): String {
    return when (error) {
        LanSession.ERROR_GAME_RUNNING -> stringResource(R.string.lan_error_game_running)
        LanSession.ERROR_DISCOVERY -> stringResource(R.string.lan_error_discovery)
        LanSession.ERROR_HOST -> stringResource(R.string.lan_error_host)
        LanSession.ERROR_JOIN -> stringResource(R.string.lan_error_join)
        else -> error
    }
}

@Composable
private fun playerStatusText(player: MelonLan.Player): String {
    if (player.isLocal) return stringResource(R.string.lan_player_you)
    return when (player.status) {
        MelonLan.PlayerStatus.HOST -> stringResource(R.string.lan_player_host)
        MelonLan.PlayerStatus.CLIENT -> stringResource(R.string.lan_player_connected)
        MelonLan.PlayerStatus.CONNECTING -> stringResource(R.string.lan_player_connecting)
        MelonLan.PlayerStatus.DISCONNECTED -> stringResource(R.string.lan_player_lost)
        MelonLan.PlayerStatus.NONE -> ""
    }
}
