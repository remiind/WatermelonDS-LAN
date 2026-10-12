// WatermelonDS-LAN: PokéCounter bridge, see PokeCounter.h

#include "PokeCounter.h"

#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

#include <atomic>
#include <cerrno>
#include <cstdio>
#include <cstring>
#include <deque>
#include <random>
#include <string>
#include <thread>

#ifdef __ANDROID__
#include <android/log.h>
#define PC_LOG(...) __android_log_print(ANDROID_LOG_INFO, "PokeCounter", __VA_ARGS__)
#else
#define PC_LOG(...) do { std::fprintf(stderr, "[PokeCounter] " __VA_ARGS__); std::fputc('\n', stderr); } while (0)
#endif

#ifndef POKECOUNTER_NO_MELONDS
#include "NDS.h"
#include "NDSCart.h"
#endif

namespace PokeCounter
{
namespace
{

constexpr uint16_t kPort = 4210;
constexpr const char* kHostName = "pokecounter.local";
constexpr int kMonSize = 236;

// --- RAM layouts per game (English addresses; other languages are shifted) --
// Values from pokebot-nds (methods/hgss.lua, pt.lua, gen_iv.lua, detect_game.lua).
struct GameLayout
{
    const char* prefix;         // first 3 chars of the game code
    const char* name;
    uint32_t anchorPtr;         // u32 at (this + lang offset) -> "anchor"
    int32_t  foeAnchorOff;      // u32 at (anchor + this) -> "foe anchor"
    int32_t  foeCountOff;       // foe anchor + this: u8 foe count
    int32_t  foeDataOff;        // foe anchor + this: 236-byte party structs
    uint32_t battleIndicator;   // u8 at (this + lang offset): 0x41 / 0x97 / 0xC0 in battle
    int32_t  trainerIdOff;      // anchor + this: u16 TID, u16 SID
    int32_t  lang[6];           // offsets for E, D, F, I, S, J
};

constexpr GameLayout kGames[] = {
    {"IPK", "HeartGold",  0x021D4158, 0x6930,  0xC14,   0xC18,   0x021E76D2, -0x23F64, {0, -0x20, 0x20, -0x60, 0x20, -0x3B08}},
    {"IPG", "SoulSilver", 0x021D4158, 0x6930,  0xC14,   0xC18,   0x021E76D2, -0x23F64, {0, -0x20, 0x20, -0x60, 0x40, -0x3B08}},
    {"CPU", "Platinum",   0x021C0794, 0x217A8, -0x2D5C, -0x2D58, 0x021D18F2, 0x8C,     {0, 0x1A0, 0x1E0, 0x160, 0x200, -0xC00}},
    {"ADA", "Diamond",    0x021C489C, 0x226FE, -0x2B74, -0x2B70, 0x021A1B2A, -0x12,    {0, 0x140, 0x180, 0xE0, 0x1A0, 0x1860}},
    {"APA", "Pearl",      0x021C489C, 0x226FE, -0x2B74, -0x2B70, 0x021A1B2A, -0x12,    {0, 0x140, 0x180, 0xE0, 0x1A0, 0x1860}},
};

// Wait this long (frames) for valid foe data after a battle started
constexpr int kFoeTimeoutFrames = 600;
// UDP retry
constexpr int kRetryFrames = 30;
constexpr int kMaxTries = 6;

struct Pending
{
    std::string json;
    int tries = 0;
    uint64_t nextFrame = 0;
};

struct State
{
    // game
    const void* cart = nullptr;
    bool supported = false;
    const GameLayout* game = nullptr;
    int32_t offset = 0;
    char gameCode[5] = {0};

    // battle tracking
    bool inBattle = false;
    bool handled = false;
    uint64_t battleStart = 0;

    // network
    int sock = -1;
    std::atomic<uint32_t> resolvedIp{0};   // network byte order, from DNS/mDNS
    uint32_t learnedIp = 0;                // from the first reply
    bool lookupStarted = false;
    std::deque<Pending> queue;

    uint64_t frame = 0;
};

State g;

// Set from any thread when a savestate (or rewind state) was loaded
std::atomic<bool> gStateLoaded{false};
std::mt19937 gRng{std::random_device{}()};

// ---------------------------------------------------------------------------
// PK4 decoding
// ---------------------------------------------------------------------------

// Order of the shuffled blocks: for each shift value the stored block index
// (1-based) that holds block A, B, C and D.
const uint8_t kBlockOrder[24][4] = {
    {1, 2, 3, 4}, {1, 2, 4, 3}, {1, 3, 2, 4}, {1, 4, 2, 3}, {1, 3, 4, 2}, {1, 4, 3, 2},
    {2, 1, 3, 4}, {2, 1, 4, 3}, {3, 1, 2, 4}, {4, 1, 2, 3}, {3, 1, 4, 2}, {4, 1, 3, 2},
    {2, 3, 1, 4}, {2, 4, 1, 3}, {3, 2, 1, 4}, {4, 2, 1, 3}, {3, 4, 1, 2}, {4, 3, 1, 2},
    {2, 3, 4, 1}, {2, 4, 3, 1}, {3, 2, 4, 1}, {4, 2, 3, 1}, {3, 4, 2, 1}, {4, 3, 2, 1},
};

inline uint32_t lcrng(uint32_t s) { return s * 0x41C64E6Du + 0x6073u; }

uint16_t rd16(ReadFn read, void* ctx, uint32_t a)
{
    return (uint16_t)(read(ctx, a) | (read(ctx, a + 1) << 8));
}

uint32_t rd32(ReadFn read, void* ctx, uint32_t a)
{
    return (uint32_t)rd16(read, ctx, a) | ((uint32_t)rd16(read, ctx, a + 2) << 16);
}

bool isMainRam(uint32_t a) { return (a & 0xFF000000u) == 0x02000000u; }

} // namespace

static bool decodeImpl(ReadFn read, void* ctx, uint32_t addr, DecodedMon& out, bool raw);

bool decodePartyMon(ReadFn read, void* ctx, uint32_t addr, DecodedMon& out)
{
    // Normally encrypted; some Platinum static encounters keep the data unencrypted
    return decodeImpl(read, ctx, addr, out, false) || decodeImpl(read, ctx, addr, out, true);
}

static bool decodeImpl(ReadFn read, void* ctx, uint32_t addr, DecodedMon& out, bool raw)
{
    const uint32_t pid = rd32(read, ctx, addr);
    const uint16_t checksum = rd16(read, ctx, addr + 6);
    if (checksum == 0)
        return false;

    // Blocks 0x08..0x87, encrypted with seed = checksum
    uint8_t stored[4][32];
    uint32_t seed = checksum;
    for (int b = 0; b < 4; b++)
    {
        for (int i = 0; i < 32; i += 2)
        {
            seed = lcrng(seed);
            uint16_t w = rd16(read, ctx, addr + 0x08 + b * 32 + i) ^ (raw ? 0 : (uint16_t)(seed >> 16));
            stored[b][i] = (uint8_t)w;
            stored[b][i + 1] = (uint8_t)(w >> 8);
        }
    }

    const uint8_t* order = kBlockOrder[((pid & 0x3E000u) >> 13) % 24];
    uint8_t data[128];   // unshuffled blocks A..D (= offsets 0x08..0x87)
    for (int b = 0; b < 4; b++)
        std::memcpy(data + b * 32, stored[order[b] - 1], 32);

    uint16_t sum = 0;
    for (int i = 0; i < 128; i += 2)
        sum = (uint16_t)(sum + (data[i] | (data[i + 1] << 8)));
    if (sum != checksum)
        return false;

    // Party stats 0x88.., encrypted with seed = PID; level is at 0x8C
    seed = pid;
    uint8_t stats[8];
    for (int i = 0; i < 8; i += 2)
    {
        seed = lcrng(seed);
        uint16_t w = rd16(read, ctx, addr + 0x88 + i) ^ (raw ? 0 : (uint16_t)(seed >> 16));
        stats[i] = (uint8_t)w;
        stats[i + 1] = (uint8_t)(w >> 8);
    }

    auto at = [&](int off) { return data[off - 0x08]; };
    out.pid = pid;
    out.species = (uint16_t)(at(0x08) | (at(0x09) << 8));
    out.otId = (uint16_t)(at(0x0C) | (at(0x0D) << 8));
    out.otSid = (uint16_t)(at(0x0E) | (at(0x0F) << 8));
    out.gender = (uint8_t)((at(0x40) >> 1) & 0x03);
    out.level = stats[0x8C - 0x88];
    out.item = (uint16_t)(at(0x0A) | (at(0x0B) << 8));
    out.ability = at(0x15);
    out.ivs = (uint32_t)(at(0x38) | (at(0x39) << 8) | (at(0x3A) << 16) | ((uint32_t)at(0x3B) << 24)) & 0x3FFFFFFFu;
    const uint16_t sv = (uint16_t)(out.otId ^ out.otSid ^ (pid >> 16) ^ (pid & 0xFFFF));
    out.shiny = sv < 8;

    return out.species >= 1 && out.species <= 493 && out.level >= 1 && out.level <= 100;
}

namespace
{

// ---------------------------------------------------------------------------
// Network
// ---------------------------------------------------------------------------

void startLookup()
{
    if (g.lookupStarted)
        return;
    g.lookupStarted = true;
    std::thread([] {
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(kHostName, nullptr, &hints, &res) == 0 && res)
        {
            auto* sin = reinterpret_cast<sockaddr_in*>(res->ai_addr);
            g.resolvedIp.store(sin->sin_addr.s_addr);
            char buf[INET_ADDRSTRLEN];
            inet_ntop(AF_INET, &sin->sin_addr, buf, sizeof(buf));
            PC_LOG("%s -> %s", kHostName, buf);
            freeaddrinfo(res);
        }
        else
        {
            PC_LOG("%s not resolvable, using UDP broadcast", kHostName);
        }
    }).detach();
}

bool ensureSocket()
{
    if (g.sock >= 0)
        return true;
    g.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (g.sock < 0)
    {
        PC_LOG("socket() failed: %s", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(g.sock, SOL_SOCKET, SO_BROADCAST, &one, sizeof(one));
    fcntl(g.sock, F_SETFL, fcntl(g.sock, F_GETFL, 0) | O_NONBLOCK);
    return true;
}

void sendDatagram(const std::string& json)
{
    if (!ensureSocket())
        return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPort);
    if (g.learnedIp)
        to.sin_addr.s_addr = g.learnedIp;
    else if (g.resolvedIp.load())
        to.sin_addr.s_addr = g.resolvedIp.load();
    else
        to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(g.sock, json.data(), json.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

void pumpNetwork()
{
    // Replies acknowledge the oldest pending message
    if (g.sock >= 0)
    {
        char buf[512];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        ssize_t n;
        while ((n = recvfrom(g.sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen)) > 0)
        {
            buf[n] = 0;
            if (!g.learnedIp)
            {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                PC_LOG("display found at %s", ip);
            }
            g.learnedIp = from.sin_addr.s_addr;
            if (!g.queue.empty() && std::strstr(buf, "\"ok\"") != nullptr)
                g.queue.pop_front();
            fromLen = sizeof(from);
        }
    }

    if (g.queue.empty())
        return;
    Pending& p = g.queue.front();
    if (g.frame < p.nextFrame)
        return;
    if (p.tries >= kMaxTries)
    {
        PC_LOG("no reply from display, dropping: %s", p.json.c_str());
        g.learnedIp = 0;   // fall back to lookup / broadcast
        g.queue.pop_front();
        return;
    }
    sendDatagram(p.json);
    p.tries++;
    p.nextFrame = g.frame + kRetryFrames;
}

void report(const DecodedMon& m)
{
    char json[224];
    const char gender = m.gender == 0 ? 'M' : (m.gender == 1 ? 'F' : 'N');
    std::snprintf(json, sizeof(json),
                  "{\"t\":\"enc\",\"sp\":%u,\"lv\":%u,\"g\":\"%c\",\"shiny\":%s,\"pid\":%u,\"eid\":%u,"
                  "\"ivs\":%u,\"ab\":%u,\"it\":%u,\"src\":\"%s\"}",
                  m.species, m.level, gender, m.shiny ? "true" : "false", m.pid, (unsigned)gRng(),
                  (unsigned)m.ivs, m.ability, m.item, g.gameCode);
    PC_LOG("wild encounter: %s", json);
    if (g.queue.size() > 16)
        g.queue.pop_front();
    Pending p;
    p.json = json;
    p.nextFrame = g.frame;
    g.queue.push_back(std::move(p));
}

// ---------------------------------------------------------------------------
// Game detection
// ---------------------------------------------------------------------------


uint8_t ramRead(void* ctx, uint32_t addr)
{
#ifndef POKECOUNTER_NO_MELONDS
    auto* nds = static_cast<melonDS::NDS*>(ctx);
    return nds->MainRAM[addr & nds->MainRAMMask];
#else
    (void)ctx; (void)addr;
    return 0;
#endif
}

} // namespace

// Core per-frame logic, separated from melonDS for testing
void processFrame(ReadFn read, void* ctx)
{
    g.frame++;
    pumpNetwork();

    // After loading a savestate the same battle can start again (e.g. a
    // static encounter right after the state). Forget the current battle so
    // it is reported again.
    if (gStateLoaded.exchange(false))
    {
        if (g.inBattle)
            PC_LOG("savestate loaded, battle tracking reset");
        g.inBattle = false;
        g.handled = false;
    }
    if (!g.supported || (g.frame & 1))
        return;

    const GameLayout& L = *g.game;
    const uint8_t ind = read(ctx, L.battleIndicator + g.offset);
    const bool battle = ind == 0x41 || ind == 0x97 || ind == 0xC0;
    if (!battle)
    {
        if (g.inBattle)
            PC_LOG("battle ended");
        g.inBattle = false;
        g.handled = false;
        return;
    }
    if (!g.inBattle)
    {
        g.inBattle = true;
        g.handled = false;
        g.battleStart = g.frame;
        PC_LOG("battle started (indicator 0x%02X)", ind);
    }
    if (g.handled)
        return;
    if (g.frame - g.battleStart > kFoeTimeoutFrames)
    {
        PC_LOG("no valid foe data found, ignoring this battle");
        g.handled = true;
        return;
    }

    const uint32_t anchor = rd32(read, ctx, L.anchorPtr + g.offset);
    if (!isMainRam(anchor))
        return;
    const uint32_t foeAnchor = rd32(read, ctx, anchor + L.foeAnchorOff);
    if (!isMainRam(foeAnchor))
        return;
    const uint8_t count = read(ctx, foeAnchor + L.foeCountOff);
    if (count == 0 || count > 6)
        return;

    DecodedMon foes[6];
    for (int i = 0; i < count; i++)
    {
        if (!decodePartyMon(read, ctx, foeAnchor + L.foeDataOff + i * kMonSize, foes[i]))
            return;   // not written completely yet, retry next time
    }
    g.handled = true;

    const uint16_t tid = rd16(read, ctx, anchor + L.trainerIdOff);
    const uint16_t sid = rd16(read, ctx, anchor + L.trainerIdOff + 2);
    for (int i = 0; i < count; i++)
    {
        // Wild Pokémon carry the player's trainer IDs, trainer Pokémon don't
        const bool wild = count <= 2 && foes[i].otId == tid && foes[i].otSid == sid;
        if (wild)
            report(foes[i]);
        else
            PC_LOG("foe #%u lv%u is a trainer's Pokemon, not counted", foes[i].species, foes[i].level);
    }
}

void onStateLoaded()
{
    gStateLoaded.store(true);
}

void resetState()
{
    g.inBattle = false;
    g.handled = false;
    g.queue.clear();
    g.learnedIp = 0;
}

void setGame(const char* code)
{
    std::memcpy(g.gameCode, code, 4);
    g.gameCode[4] = 0;
    g.supported = false;
    g.game = nullptr;
    static const char kLangs[] = "EDFISJ";
    const char* lp = std::strchr(kLangs, code[3]);
    if (!lp || !code[3])
        return;
    for (const GameLayout& L : kGames)
    {
        if (std::strncmp(code, L.prefix, 3) == 0)
        {
            g.game = &L;
            g.offset = L.lang[lp - kLangs];
            g.supported = true;
            return;
        }
    }
}

#ifndef POKECOUNTER_NO_MELONDS
void onFrame(melonDS::NDS& nds)
{
    const void* cart = nds.NDSCartSlot.GetCart();
    if (cart != g.cart)
    {
        g.cart = cart;
        g.supported = false;
        g.inBattle = false;
        g.handled = false;
        if (cart)
        {
            const auto& hdr = nds.NDSCartSlot.GetCart()->GetHeader();
            setGame(hdr.GameCode);
            if (g.supported)
            {
                PC_LOG("%s detected (%s, offset %d), encounter reporting active", g.game->name, g.gameCode, (int)g.offset);
                startLookup();
            }
        }
    }
    if (!g.supported && g.queue.empty())
        return;
    processFrame(ramRead, &nds);
}
#endif

}
