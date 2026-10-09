#ifndef POKECOUNTER_H
#define POKECOUNTER_H

// WatermelonDS-LAN: PokéCounter bridge.
//
// Watches the emulated RAM of Pokémon HeartGold / SoulSilver for wild
// battles and reports every wild encounter as a small JSON datagram (UDP,
// port 4210) to a PokéCounter desk display on the local network.
//
//  * Only active while a HGSS cartridge (game code IPK* / IPG*) is running.
//  * Called from the emulator thread once per emulated frame; does no
//    blocking I/O there (non-blocking UDP socket, name lookup on a helper
//    thread).
//  * The display is found via "pokecounter.local" or, if that does not
//    resolve, via UDP broadcast. The first reply pins its IP address.
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
};

// Decrypts a 236-byte Gen 4 party structure at addr. Returns false if the
// checksum does not match (data not written yet or garbage).
bool decodePartyMon(ReadFn read, void* ctx, uint32_t addr, DecodedMon& out);

// Selects the game by its 4-character game code (e.g. "IPGD" = SoulSilver DE).
void setGame(const char* gameCode);
// One emulated frame of battle tracking, reading RAM through `read`.
void processFrame(ReadFn read, void* ctx);
void resetState();

}

#endif // POKECOUNTER_H
