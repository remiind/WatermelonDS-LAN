# WatermelonDS-LAN (unofficial fork)

> **This is an unofficial, experimental fork** of [WatermelonDS](https://github.com/SapphireRhodonite/WatermelonDS)
> that adds LAN multiplayer (DS local wireless over Wi-Fi). It is **not affiliated with, endorsed by or supported by**
> the WatermelonDS developer (SapphireRhodonite), the melonDS Android port (rafaelvcaetano), the melonDS team or
> Nintendo. Please **do not report problems with this fork to any of the upstream projects** – open an issue here instead.

## What this fork adds

* **LAN multiplayer**: host or join a session from the ROM list menu (*⋮ → LAN multiplayer*), then start the same game
  on every device. This uses melonDS' own LAN implementation (ENet), so local wireless features such as Pokémon trades
  and battles in the Union Room can work between devices on the same Wi-Fi.
* Sessions are found automatically (UDP broadcast, port 7063); joining by IP address is possible as a fallback.
  Gameplay traffic uses UDP port 7064.
* **PokéCounter bridge** (optional companion): while Pokémon HeartGold or SoulSilver runs, every wild encounter
  (species, level, gender, shiny, PID) is sent as a small JSON datagram over UDP port 4210 to a
  PokéCounter desk display (ESP32) on the same network. It is only active for HGSS cartridges,
  finds the display via `pokecounter.local` or UDP broadcast and does nothing else. RAM layout taken from
  [pokebot-nds](https://github.com/wyanido/pokebot-nds) (MIT).
* Test builds are published as the pre-release [`lan-latest`](../../releases/tag/lan-latest). They use the app id
  `me.magnum.melondualds.nightly`, so they install **next to** a regular WatermelonDS without touching its data.
  The build workflow additionally produces a variant with the upstream app id for the maintainer's own use with
  launcher frontends. It is only kept as a short-lived workflow artifact and is **not** a release.

Status: **experimental**. Keep backups of your save files before trading or battling.

## Changes compared to upstream (GPLv3 §5a notice)

Modified by remiind, starting 2026-10-08, based on upstream commit `bb26729` (2026-10-01):

| File | Change |
| --- | --- |
| `app/CMakeLists.txt` | Enables melonDS' ENet-based LAN interface on Android, adds `MelonLan.cpp` and `PokeCounter.cpp` and builds ENet with its packet throttling disabled (Wi-Fi jitter made it drop most DS wireless frames) |
| `app/src/main/cpp/MelonDS.cpp` | Emulator loop drives the multiplayer interface through `MelonLan::loopProcess()` |
| `app/src/main/cpp/MelonLan.cpp/.h` | New: JNI bridge for host / join / discovery / player list |
| `app/src/main/cpp/PokeCounter.cpp/.h` | New: reports wild HGSS encounters to a PokéCounter display (UDP 4210) |
| `app/src/main/cpp/MelonInstance.cpp` | Calls `PokeCounter::onFrame()` after every emulated frame |
| `app/src/main/java/me/magnum/melonds/lan/` | New: `MelonLan` bindings and `LanSession` (lobby poller, Wi-Fi multicast lock) |
| `app/src/main/java/me/magnum/melonds/ui/lan/` | New: LAN lobby screen |
| `app/src/main/java/me/magnum/melonds/ui/romlist/…` | Menu entry for the LAN lobby |
| `app/src/main/AndroidManifest.xml` | Lobby activity, `CHANGE_WIFI_MULTICAST_STATE` and `WAKE_LOCK` permissions (Wi-Fi low-latency mode during sessions) |
| `app/src/main/res/values*/strings_lan.xml` | English and German texts for the lobby |
| `app/src/main/assets/licenses/enet.txt` | ENet license (now linked into the app) |
| `.github/workflows/*` | Upstream workflows only run in the upstream repository; `lan-build.yaml` builds this fork |
| `.github/lan/lan-test.keystore` | Throwaway signing key for test builds (see below) |

The full history of every change is available in the git log of this repository.

## License

This fork is distributed under the **GNU General Public License v3.0**, like WatermelonDS, the melonDS Android port and
melonDS (see [LICENSE](LICENSE)). The complete corresponding source code of every published build is this repository
at the tagged commit, including its git submodules. Third-party components keep their own licenses, e.g.
[ENet](https://github.com/lsalzman/enet) (MIT, copyright Lee Salzman), whose license is bundled with the app.

All credit for the emulator itself goes to the authors listed under [Credits](#credits).

## No games, BIOS or firmware included

This project contains **no** Nintendo games, BIOS or firmware files and does not link to any. Use only dumps of games and
firmware you own. "Nintendo DS", "Nintendo DSi" and "Pokémon" are trademarks of their respective owners and are used
here only to describe compatibility.

## Signing key of the test builds

Test builds are signed with a key that is **deliberately public** (`.github/lan/lan-test.keystore`). It only exists so that
newer test builds install over older ones. It does **not** prove who built an APK – only install builds you downloaded
from this repository's releases page.

## Use of AI

The LAN multiplayer changes in this fork were written with substantial help from an AI coding assistant
(Claude by Anthropic). Commits produced this way carry a `Co-Authored-By: Claude` trailer.
The emulator core and everything inherited from upstream are unaffected by this note.

## Funding links

The "Sponsor" links of this repository are inherited from upstream and support the **original WatermelonDS developer**,
not this fork.

---

*The original WatermelonDS README follows.*

# WatermelonDS
A Nintendo DS and DSi emulator for Android, built on top of [melonDS](https://melonds.kuribo64.net/) and the
[melonDS Android port](https://github.com/rafaelvcaetano/melonDS-android) by rafaelvcaetano.

WatermelonDS is a fork that keeps up with upstream while adding RetroAchievements (including offline play), a Vulkan
renderer, RetroArch shader presets and full external display support, wrapped in its own visual identity.



[<img src="https://raw.githubusercontent.com/Kunzisoft/Github-badge/main/get-it-on-github.png" alt="Get it on GitHub" height="80">](https://github.com/SapphireRhodonite/melonDS-android/releases/latest)

<p align="center">
   <img width="450" height="400" alt="WaterMelon1" src="https://github.com/user-attachments/assets/187ea254-877e-4efd-a8dd-93a6023684ad" />
   <img width="450" height="400" alt="WaterMelon1" src="https://github.com/user-attachments/assets/01e82877-a138-44a3-bc71-c24d48cabab6" />
   <img width="450" height="400" alt="WaterMelon1" src="https://github.com/user-attachments/assets/27868058-ce89-470d-afac-b812dea5d1de" />
</p>


# What WatermelonDS adds
Everything from the upstream Android port, plus:

### RetroAchievements
*  Achievements, leaderboards and Hardcore mode, with a shared card style for the in-game popups
*  Your profile and score are shown on load and in the settings screen
*  **Offline play**: unlocks earned without a connection are stored and submitted later. Two providers are available
   the one built into WatermelonDS, or [RAOfflineProxy](https://github.com/SapphireRhodonite) if you prefer to run it
   yourself. Note that RAOfflineProxy does not support Hardcore mode
*  Achievements can be disabled without logging out, and sessions start even for games that have no achievement set

### Renderers
*  Software, OpenGL, Vulkan and compute renderers, with internal resolution scaling
*  Vulkan gained a *fastpath* presentation profile, optimized texture uploads and dynamic scissor clipping
*  **RetroArch shader presets** on every renderer, through [librashader](https://github.com/SnowflakePowered/librashader)
*  Adrenotools driver support on the `gitHub` flavor, for custom Adreno drivers

### External displays
*  Play on a second screen (TV, dock, devices such as the Odin 2), with the touch screen still usable on the handheld
*  Per-ROM external screen and layout settings, screen rotation, backgrounds, video filtering and aspect ratio control
*  Achievements are shown on the external display while you browse your ROM list

### Library and layouts
*  DSiWare titles in the ROM list, box art on the "continue" shelf, and custom ROM names
*  Layout editor with screen transparency, aspect ratio options and separate centering controls
*  Settings backup and restore (requires a restart to apply), with internal and external layouts backed up separately
*  Alternate input bindings and a hold-to-fast-forward button

The core is kept in sync with melonDS 1.0 and later upstream changes.

# Missing Features
*  Local Multiplayer
*  DSi SD card support
*  Customizable button skins

# Performance
Performance is solid on 64 bit devices with thread rendering and JIT enabled, and should run at full speed on flagship
devices. Performance on older devices, specially 32 bit devices, is very poor due to the lack of JIT support.

# Integration with third-party frontends
It's possible to launch WatermelonDS from third party frontends. For that, you will need to have the ROMs you want to
launch already scanned by WatermelonDS. Then, you can configure your third-party frontend with the following
configuration:
*  Package name: `me.magnum.melondualds` (nightly builds use `me.magnum.melondualds.nightly`)
*  Activity name: `me.magnum.melonds.ui.emulator.EmulatorActivity`
*  Parameters (choose one):
    * Intent data (preferred) - a URI of the NDS ROM (ZIP and 7z files are supported). Ensure [read permission is granted](https://developer.android.com/reference/android/content/Intent#FLAG_GRANT_READ_URI_PERMISSION)
    * `uri` (deprecated) - a string with the [SAF](https://developer.android.com/guide/topics/providers/create-document-provider) URI of the NDS ROM (ZIP and 7z files are supported)
    * `PATH` (deprecated) - a string with the absolute path to the NDS ROM (ZIP and 7z files are supported)


### Info regarding save files
When launching ROMs from third-party frontends, if WatermelonDS hasn't scanned that particular ROM previously, it won't
be able to create the save file next to the ROM file if the option "Save next to ROM file" is enabled in the settings or
the save file directory is not set. Instead, WatermelonDS will create a save file in
`Android/data/me.magnum.melondualds/files/saves`

# Releases

Builds are published [here](https://github.com/SapphireRhodonite/melonDS-android/releases). Release candidates are
marked as pre-releases; they can contain more bugs than usual and you may need to clear your app data to get them to
work properly after an update.

# Building
To build the project you will need the Android SDK, NDK, CMake, a JDK 21 toolchain and a Rust toolchain
(`cargo` and `rustup`), which is used to build librashader for each ABI.

## Build steps:
1.  Clone the project, including submodules with:
    
    `git clone --recurse-submodules https://github.com/SapphireRhodonite/melonDS-android.git`
2.  Install the Android SDK, NDK (`28.0.13004108`) and CMake
3.  Install Rust with [rustup](https://rustup.rs/). If Android Studio does not inherit your shell `PATH`, set the
    `CARGO` and `RUSTUP` environment variables to the executable paths
4.  Build with:
    1.  Unix: `./gradlew :app:assembleGitHubProdDebug`
    2.  Windows: `gradlew.bat :app:assembleGitHubProdDebug`
5.  The generated APK can be found at `app/build/outputs/apk/gitHubProd/debug`

There are two flavor dimensions, so build tasks combine both: `gitHub` or `playStore` (the former enables adrenotools)
and `prod` or `nightly`. For example, `:app:assembleGitHubNightlyDebug` or `:app:assembleGitHubProdRelease`.

If you want to create a release build, you will need to modify your `local.properties` file to include the following fields:  
*  `MELONDS_KEYSTORE=<path_to_your_keystore>`
*  `MELONDS_KEYSTORE_PASSWORD=<keystore_password>`
*  `MELONDS_KEY_ALIAS=<name_of_your_key_alias>`
*  `MELONDS_KEY_PASSWORD=<key_alias_password>`

# Credits
*  [melonDS](https://github.com/melonDS-emu/melonDS) by Arisotura and contributors — the emulator core
*  [melonDS Android](https://github.com/rafaelvcaetano/melonDS-android) by rafaelvcaetano
*  [librashader](https://github.com/SnowflakePowered/librashader) — RetroArch shader preset support
*  [rcheevos](https://github.com/RetroAchievements/rcheevos) — RetroAchievements support
