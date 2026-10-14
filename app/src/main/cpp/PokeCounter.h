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
void markStateLoaded(Tracker& t);

}

#endif // POKECOUNTER_H
