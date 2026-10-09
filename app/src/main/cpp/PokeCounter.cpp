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

// --- HGSS RAM layout (US version; other languages are shifted) --------------
constexpr uint32_t kAnchorPtr       = 0x021D4158;   // -> "anchor"
constexpr uint32_t kBattleIndicator = 0x021E76D2;   // u8, 0x41 / 0x97 / 0xC0 while in battle
constexpr uint32_t kFoeAnchorOff    = 0x6930;       // anchor + off -> foe anchor
constexpr uint32_t kFoeCountOff     = 0xC14;        // foe anchor + off: u8 foe count
constexpr uint32_t kFoeDataOff      = 0xC18;        // foe anchor + off: party structs
constexpr uint32_t kTrainerIdOff    = 0x23F64;      // anchor - off: u16 TID, u16 SID

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

bool decodePartyMon(ReadFn read, void* ctx, uint32_t addr, DecodedMon& out)
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
            uint16_t w = rd16(read, ctx, addr + 0x08 + b * 32 + i) ^ (uint16_t)(seed >> 16);
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
        uint16_t w = rd16(read, ctx, addr + 0x88 + i) ^ (uint16_t)(seed >> 16);
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
    char json[160];
    const char gender = m.gender == 0 ? 'M' : (m.gender == 1 ? 'F' : 'N');
    std::snprintf(json, sizeof(json),
                  "{\"t\":\"enc\",\"sp\":%u,\"lv\":%u,\"g\":\"%c\",\"shiny\":%s,\"pid\":%u,\"src\":\"%s\"}",
                  m.species, m.level, gender, m.shiny ? "true" : "false", m.pid, g.gameCode);
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

bool languageOffset(char version, char lang, int32_t& off)
{
    // pokebot-nds: offsets relative to the English release
    switch (lang)
    {
        case 'E': off = 0; return true;
        case 'D': off = -0x20; return true;
        case 'F': off = 0x20; return true;
        case 'I': off = -0x60; return true;
        case 'S': off = version == 'G' ? 0x40 : 0x20; return true;   // SS / HG
        case 'J': off = -0x3B08; return true;
        default: return false;
    }
}

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
    if (!g.supported || (g.frame & 1))
        return;

    const uint8_t ind = read(ctx, kBattleIndicator + g.offset);
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

    const uint32_t anchor = rd32(read, ctx, kAnchorPtr + g.offset);
    if (!isMainRam(anchor))
        return;
    const uint32_t foeAnchor = rd32(read, ctx, anchor + kFoeAnchorOff);
    if (!isMainRam(foeAnchor))
        return;
    const uint8_t count = read(ctx, foeAnchor + kFoeCountOff);
    if (count == 0 || count > 6)
        return;

    DecodedMon foes[6];
    for (int i = 0; i < count; i++)
    {
        if (!decodePartyMon(read, ctx, foeAnchor + kFoeDataOff + i * kMonSize, foes[i]))
            return;   // not written completely yet, retry next time
    }
    g.handled = true;

    const uint16_t tid = rd16(read, ctx, anchor - kTrainerIdOff);
    const uint16_t sid = rd16(read, ctx, anchor - kTrainerIdOff + 2);
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
    g.supported = code[0] == 'I' && code[1] == 'P' && (code[2] == 'K' || code[2] == 'G')
        && languageOffset(code[2], code[3], g.offset);
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
                PC_LOG("HGSS detected (%s, offset %d), encounter reporting active", g.gameCode, g.offset);
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
