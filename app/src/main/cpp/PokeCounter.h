#ifndef POKECOUNTER_H
#define POKECOUNTER_H

// WatermelonDS-LAN: PokéCounter bridge.
//
// Watches the emulated RAM of Pokémon HeartGold / SoulSilver, Platinum,
// Diamond and Pearl for wild
// battles and reports every wild encounter as a small JSON datagram (UDP,
// port 4210) to a PokéCounter desk display on the local network.
//
//  * Only active while one of these cartridges (IPK*, IPG*, CPU*, ADA*, APA*)
//    is running. The game code is sent along ("src") so the display can keep
//    separate statistics per edition.
//  * Every emulator instance (NDS object) gets its own tracker, so several
//    instances in one process (desktop melonDS) each report their own
//    encounters.
//  * Called from the emulator thread once per emulated frame; does no
//    blocking I/O there (non-blocking UDP socket, name lookup on a helper
//    thread).
//  * Reports also carry "tsv" = TID ^ SID of the player, so the display
//    can show each encounter's shiny value.
//  * Every report carries a random event id ("eid") so the display can
//    drop UDP retries without merging identical encounters (a static
//    encounter after a savestate has the same PID every time).
//  * The display is found via the environment variable POKECOUNTER_HOST,
//    "pokecounter.local" (system resolver, then a direct mDNS query) or,
//    if that does not resolve, via UDP broadcast. The first reply pins its
//    IP address.
//
// RAM layout (pointers and language offsets) taken from pokebot-nds by
// wyanido (MIT License, https://github.com/wyanido/pokebot-nds).

#include <cstdint>
#include <cstddef>

namespace melonDS { class NDS; }

namespace PokeCounter
{

// Called by the emulator thread after every emulated frame.
void onFrame(melonDS::NDS& nds);

// Called after a savestate or rewind state was loaded (any thread).
void onStateLoaded(melonDS::NDS& nds);

// Optional: called before an emulator instance is destroyed.
void onInstanceGone(melonDS::NDS& nds);

// ---- shiny auto-pause ------------------------------------------------------
//
// When a shiny wild Pokémon is reported, the instance's tracker raises
//  * a one-shot "shiny event": the frontend pauses that instance, and
//  * a "shiny hold": the frontend refuses reset and savestate loading for
//    that instance until the user resumes it by hand.
// Enabled by default; POKECOUNTER_AUTOPAUSE=0 switches it off.

bool autoPauseEnabled();
void setAutoPauseEnabled(bool on);

// ---- team ------------------------------------------------------------------
// Outside battles the party is read about once a second; every Pokémon whose
// data changed is sent as {"t":"mon",...} (IVs, EVs, stats, moves, nature,
// ability, item, nickname) for the display's Team tab. On by default,
// POKECOUNTER_TEAM=0 switches it off.
bool teamEnabled();
void setTeamEnabled(bool on);

// ---- PC boxes ---------------------------------------------------------------
// The 18 boxes of the last in-game save (save memory, not RAM) are checked
// every 2 s; changed boxes are sent as {"t":"box",...} (half a box per
// packet) for the display's Boxes tab. Instances running the same game send a
// box only once. On by default, POKECOUNTER_BOXES=0 switches it off.
bool boxesEnabled();

// Emulator thread, right after onFrame(): true once per shiny, as soon as the
// display confirmed the report (at most 1.5 s later), so it still hears about it.
bool takeShinyEvent(melonDS::NDS& nds);
bool shinyHold(melonDS::NDS& nds);
void releaseShinyHold(melonDS::NDS& nds);

// Auto-hunt (AutoHunt.h): number of wild Pokémon reported by this instance so
// far, whether the last one was shiny, and whether the running game is one of
// the supported ones.
uint32_t encounterSeq(melonDS::NDS& nds);
bool lastEncounterShiny(melonDS::NDS& nds);
bool gameSupported(melonDS::NDS& nds);

// For frontends with a single emulator instance (Android).
bool takeAnyShinyEvent();
bool anyShinyHold();
void releaseAllShinyHolds();

// ---- egg hunt (Masuda method) -------------------------------------------------
// Save on the long path next to the day care (HGSS: Route 34, D/P/Pt: Solaceon
// Town) with the parents left there, free slots in the party and the bike
// registered on Y. The hunt resets, bikes up and down, fetches every egg and
// checks it right away (the PID is fixed in Gen 4 when the egg is made, so no
// hatching is needed), reports every egg like an encounter and stops on a
// shiny egg; with a full party it resets and starts over.
enum class EggStep { None, Reset, Stopped };
enum class EggStop { None, ByUser, Shiny, NotInGame, NoBike, Blocked, NoEggs };
// Emulator thread. eggStart returns an error text or nullptr (then the caller
// resets the console).
const char* eggStart(melonDS::NDS& nds);
void eggStop(melonDS::NDS& nds);
bool eggActive(melonDS::NDS& nds);
uint32_t eggKeys(melonDS::NDS& nds);          // DS key bits to press in the next frame
EggStep eggAfterFrame(melonDS::NDS& nds);     // after onFrame()
EggStop eggStopReason(melonDS::NDS& nds);
uint32_t eggCycles(melonDS::NDS& nds);        // rounds without a shiny
uint32_t eggHatched(melonDS::NDS& nds);       // eggs checked

// ---- exposed for unit tests ------------------------------------------------

using ReadFn = uint8_t (*)(void* ctx, uint32_t addr);

struct DecodedMon
{
    uint32_t pid = 0;
    uint16_t species = 0;
    uint16_t otId = 0;
    uint16_t otSid = 0;
    uint8_t level = 0;
    uint8_t gender = 2;   // 0 = male, 1 = female, 2 = genderless
    bool shiny = false;
    uint32_t ivs = 0;     // packed: HP | Atk<<5 | Def<<10 | Spe<<15 | SpA<<20 | SpD<<25
    uint16_t item = 0;    // held item (Gen 4 item index)
    uint8_t ability = 0;  // ability id
};

// Decrypts a 236-byte Gen 4 party structure at addr. Returns false if the
// checksum does not match (data not written yet or garbage).
bool decodePartyMon(ReadFn read, void* ctx, uint32_t addr, DecodedMon& out);

// Per-instance state (opaque outside PokeCounter.cpp)
struct Tracker;
Tracker* newTracker();
void deleteTracker(Tracker* t);

// Selects the game by its 4-character game code (e.g. "IPGD" = SoulSilver DE).
void setGame(Tracker& t, const char* gameCode);
// One emulated frame of battle tracking, reading RAM through `read`.
void processFrame(Tracker& t, ReadFn read, void* ctx);
void resetState(Tracker& t);
// Egg hunt core (see above), reading RAM through `read`
const char* eggStart(Tracker& t, ReadFn read, void* ctx);
void eggStop(Tracker& t);
bool eggActive(const Tracker& t);
uint32_t eggKeys(const Tracker& t);
EggStep eggAfterFrame(Tracker& t, ReadFn read, void* ctx);
EggStop eggStopReason(const Tracker& t);
uint32_t eggCycles(const Tracker& t);
uint32_t eggHatched(const Tracker& t);
// Checks the PC boxes in a DS save image (at most every 2 s, see above).
void processSave(Tracker& t, const uint8_t* save, uint32_t len);
// Level of a Pokémon from its experience points (Gen 4 growth rates).
int levelFromExp(int species, uint32_t exp);
void markStateLoaded(Tracker& t);
bool takeShinyEvent(Tracker& t);
bool shinyHold(Tracker& t);
void releaseShinyHold(Tracker& t);

}

#endif // POKECOUNTER_H
