// WatermelonDS-LAN: PokéCounter bridge, see PokeCounter.h

#include "PokeCounter.h"

#ifdef _WIN32
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <winsock2.h>
#include <ws2tcpip.h>
#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#endif
using SockT = SOCKET;
static const SockT kNoSock = INVALID_SOCKET;
static void closeSock(SockT s) { closesocket(s); }
static void setNonBlocking(SockT s) { u_long one = 1; ioctlsocket(s, FIONBIO, &one); }
#else
#include <arpa/inet.h>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
using SockT = int;
static const SockT kNoSock = -1;
static void closeSock(SockT s) { close(s); }
static void setNonBlocking(SockT s) { fcntl(s, F_SETFL, fcntl(s, F_GETFL, 0) | O_NONBLOCK); }
#endif

#include <atomic>
#include <cerrno>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
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
// UDP retry (wall clock, so fast forward does not shorten it)
constexpr int kRetryMs = 400;
constexpr int kMaxTries = 8;

uint64_t nowMs()
{
    using namespace std::chrono;
    return (uint64_t)duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

struct Pending
{
    std::string json;
    int tries = 0;
    uint64_t nextMs = 0;
};

} // namespace

// One tracker per emulator instance (the desktop version runs several
// instances in one process, each on its own thread).
struct Tracker
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
    SockT sock = kNoSock;
    std::shared_ptr<std::atomic<uint32_t>> resolvedIp = std::make_shared<std::atomic<uint32_t>>(0);   // network byte order
    uint32_t learnedIp = 0;                // from the first reply
    bool lookupStarted = false;
    std::deque<Pending> queue;

    uint64_t frame = 0;

    // Set from any thread when a savestate (or rewind state) was loaded
    std::atomic<bool> stateLoaded{false};

    ~Tracker()
    {
        if (sock != kNoSock)
            closeSock(sock);
    }
};

namespace
{

std::mt19937 gRng{std::random_device{}()};
std::mutex gRngLock;

uint32_t randomEid()
{
    std::lock_guard<std::mutex> lk(gRngLock);
    return (uint32_t)gRng();
}

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

bool netInit()
{
#ifdef _WIN32
    static const bool ok = [] {
        WSADATA wsa;
        return WSAStartup(MAKEWORD(2, 2), &wsa) == 0;
    }();
    return ok;
#else
    return true;
#endif
}

// Asks for a ".local" name directly via multicast DNS (for systems whose
// resolver does not do mDNS). Returns the address in network byte order or 0.
uint32_t mdnsLookup(const char* name)
{
    uint8_t q[256] = {0, 0, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0};
    size_t n = 12;
    const char* p = name;
    while (*p && n < 200)
    {
        const char* dot = std::strchr(p, '.');
        size_t len = dot ? (size_t)(dot - p) : std::strlen(p);
        q[n++] = (uint8_t)len;
        std::memcpy(q + n, p, len);
        n += len;
        p += len + (dot ? 1 : 0);
        if (!dot)
            break;
    }
    const uint8_t tail[] = {0, 0, 1, 0x80, 1};   // root, type A, class IN + unicast reply
    std::memcpy(q + n, tail, sizeof(tail));
    n += sizeof(tail);

    SockT s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == kNoSock)
        return 0;
#ifdef _WIN32
    DWORD tv = 1500;
#else
    timeval tv{1, 500000};
#endif
    setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&tv), sizeof(tv));
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(5353);
    inet_pton(AF_INET, "224.0.0.251", &to.sin_addr);
    uint32_t ip = 0;
    if (sendto(s, reinterpret_cast<const char*>(q), (int)n, 0, reinterpret_cast<sockaddr*>(&to), sizeof(to)) > 0)
    {
        uint8_t buf[1500];
        sockaddr_in from{};
        socklen_t fl = sizeof(from);
        int r = (int)recvfrom(s, reinterpret_cast<char*>(buf), sizeof(buf), 0, reinterpret_cast<sockaddr*>(&from), &fl);
        // Response with an answer: use the sender (the display answers for itself)
        if (r > 12 && (buf[2] & 0x80) && (buf[6] | buf[7]))
            ip = from.sin_addr.s_addr;
    }
    closeSock(s);
    return ip;
}

void startLookup(Tracker& t)
{
    if (t.lookupStarted)
        return;
    t.lookupStarted = true;
    auto out = t.resolvedIp;   // shared, survives the tracker
    std::thread([out] {
        // POKECOUNTER_HOST overrides the name (IP or host name)
        const char* env = std::getenv("POKECOUNTER_HOST");
        const char* host = env && *env ? env : kHostName;
        uint32_t ip = 0;
        addrinfo hints{};
        hints.ai_family = AF_INET;
        hints.ai_socktype = SOCK_DGRAM;
        addrinfo* res = nullptr;
        if (getaddrinfo(host, nullptr, &hints, &res) == 0 && res)
        {
            ip = reinterpret_cast<sockaddr_in*>(res->ai_addr)->sin_addr.s_addr;
            freeaddrinfo(res);
        }
        if (!ip && std::strstr(host, ".local"))
            ip = mdnsLookup(host);
        if (ip)
        {
            out->store(ip);
            char buf[INET_ADDRSTRLEN];
            in_addr a{};
            a.s_addr = ip;
            inet_ntop(AF_INET, &a, buf, sizeof(buf));
            PC_LOG("%s -> %s", host, buf);
        }
        else
        {
            PC_LOG("%s not resolvable, using UDP broadcast", host);
        }
    }).detach();
}

bool ensureSocket(Tracker& t)
{
    if (t.sock != kNoSock)
        return true;
    if (!netInit())
        return false;
    t.sock = socket(AF_INET, SOCK_DGRAM, 0);
    if (t.sock == kNoSock)
    {
        PC_LOG("socket() failed: %s", strerror(errno));
        return false;
    }
    int one = 1;
    setsockopt(t.sock, SOL_SOCKET, SO_BROADCAST, reinterpret_cast<const char*>(&one), sizeof(one));
    setNonBlocking(t.sock);
    return true;
}

void sendDatagram(Tracker& t, const std::string& json)
{
    if (!ensureSocket(t))
        return;
    sockaddr_in to{};
    to.sin_family = AF_INET;
    to.sin_port = htons(kPort);
    if (t.learnedIp)
        to.sin_addr.s_addr = t.learnedIp;
    else if (t.resolvedIp->load())
        to.sin_addr.s_addr = t.resolvedIp->load();
    else
        to.sin_addr.s_addr = INADDR_BROADCAST;
    sendto(t.sock, json.data(), (int)json.size(), 0, reinterpret_cast<sockaddr*>(&to), sizeof(to));
}

void pumpNetwork(Tracker& t)
{
    // Replies acknowledge the oldest pending message
    if (t.sock != kNoSock)
    {
        char buf[512];
        sockaddr_in from{};
        socklen_t fromLen = sizeof(from);
        int n;
        while ((n = (int)recvfrom(t.sock, buf, sizeof(buf) - 1, 0, reinterpret_cast<sockaddr*>(&from), &fromLen)) > 0)
        {
            buf[n] = 0;
            if (!t.learnedIp)
            {
                char ip[INET_ADDRSTRLEN];
                inet_ntop(AF_INET, &from.sin_addr, ip, sizeof(ip));
                PC_LOG("display found at %s", ip);
            }
            t.learnedIp = from.sin_addr.s_addr;
            if (!t.queue.empty() && std::strstr(buf, "\"ok\"") != nullptr)
                t.queue.pop_front();
            fromLen = sizeof(from);
        }
    }

    if (t.queue.empty())
        return;
    Pending& p = t.queue.front();
    const uint64_t now = nowMs();
    if (now < p.nextMs)
        return;
    if (p.tries >= kMaxTries)
    {
        PC_LOG("no reply from display, dropping: %s", p.json.c_str());
        t.learnedIp = 0;   // fall back to lookup / broadcast
        t.queue.pop_front();
        return;
    }
    sendDatagram(t, p.json);
    p.tries++;
    p.nextMs = now + kRetryMs;
}

void report(Tracker& t, const DecodedMon& m)
{
    char json[224];
    const char gender = m.gender == 0 ? 'M' : (m.gender == 1 ? 'F' : 'N');
    std::snprintf(json, sizeof(json),
                  "{\"t\":\"enc\",\"sp\":%u,\"lv\":%u,\"g\":\"%c\",\"shiny\":%s,\"pid\":%u,\"eid\":%u,"
                  "\"ivs\":%u,\"ab\":%u,\"it\":%u,\"src\":\"%s\"}",
                  m.species, m.level, gender, m.shiny ? "true" : "false", m.pid, (unsigned)randomEid(),
                  (unsigned)m.ivs, m.ability, m.item, t.gameCode);
    PC_LOG("wild encounter: %s", json);
    if (t.queue.size() > 16)
        t.queue.pop_front();
    Pending p;
    p.json = json;
    p.nextMs = 0;
    t.queue.push_back(std::move(p));
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
void processFrame(Tracker& t, ReadFn read, void* ctx)
{
    t.frame++;
    pumpNetwork(t);

    // After loading a savestate the same battle can start again (e.t. a
    // static encounter right after the state). Forget the current battle so
    // it is reported again.
    if (t.stateLoaded.exchange(false))
    {
        if (t.inBattle)
            PC_LOG("savestate loaded, battle tracking reset");
        t.inBattle = false;
        t.handled = false;
    }
    if (!t.supported || (t.frame & 1))
        return;

    const GameLayout& L = *t.game;
    const uint8_t ind = read(ctx, L.battleIndicator + t.offset);
    const bool battle = ind == 0x41 || ind == 0x97 || ind == 0xC0;
    if (!battle)
    {
        if (t.inBattle)
            PC_LOG("battle ended");
        t.inBattle = false;
        t.handled = false;
        return;
    }
    if (!t.inBattle)
    {
        t.inBattle = true;
        t.handled = false;
        t.battleStart = t.frame;
        PC_LOG("battle started (indicator 0x%02X)", ind);
    }
    if (t.handled)
        return;
    if (t.frame - t.battleStart > kFoeTimeoutFrames)
    {
        PC_LOG("no valid foe data found, ignoring this battle");
        t.handled = true;
        return;
    }

    const uint32_t anchor = rd32(read, ctx, L.anchorPtr + t.offset);
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
    t.handled = true;

    const uint16_t tid = rd16(read, ctx, anchor + L.trainerIdOff);
    const uint16_t sid = rd16(read, ctx, anchor + L.trainerIdOff + 2);
    for (int i = 0; i < count; i++)
    {
        // Wild Pokémon carry the player's trainer IDs, trainer Pokémon don't
        const bool wild = count <= 2 && foes[i].otId == tid && foes[i].otSid == sid;
        if (wild)
            report(t, foes[i]);
        else
            PC_LOG("foe #%u lv%u is a trainer's Pokemon, not counted", foes[i].species, foes[i].level);
    }
}

Tracker* newTracker() { return new Tracker(); }
void deleteTracker(Tracker* t) { delete t; }
void markStateLoaded(Tracker& t) { t.stateLoaded.store(true); }

void resetState(Tracker& t)
{
    t.inBattle = false;
    t.handled = false;
    t.queue.clear();
    t.learnedIp = 0;
}

void setGame(Tracker& t, const char* code)
{
    std::memcpy(t.gameCode, code, 4);
    t.gameCode[4] = 0;
    t.supported = false;
    t.game = nullptr;
    static const char kLangs[] = "EDFISJ";
    const char* lp = std::strchr(kLangs, code[3]);
    if (!lp || !code[3])
        return;
    for (const GameLayout& L : kGames)
    {
        if (std::strncmp(code, L.prefix, 3) == 0)
        {
            t.game = &L;
            t.offset = L.lang[lp - kLangs];
            t.supported = true;
            return;
        }
    }
}

// ---- instance registry --------------------------------------------------------

namespace
{
std::mutex gTrackersLock;
std::map<const void*, std::unique_ptr<Tracker>> gTrackers;

Tracker& trackerFor(const void* key)
{
    std::lock_guard<std::mutex> lk(gTrackersLock);
    auto& p = gTrackers[key];
    if (!p)
        p = std::make_unique<Tracker>();
    return *p;
}
}

void onStateLoaded(melonDS::NDS& nds)
{
    trackerFor(&nds).stateLoaded.store(true);
}

void onInstanceGone(melonDS::NDS& nds)
{
    std::lock_guard<std::mutex> lk(gTrackersLock);
    gTrackers.erase(&nds);
}

#ifndef POKECOUNTER_NO_MELONDS
void onFrame(melonDS::NDS& nds)
{
    Tracker& t = trackerFor(&nds);
    const void* cart = nds.NDSCartSlot.GetCart();
    if (cart != t.cart)
    {
        t.cart = cart;
        t.supported = false;
        t.inBattle = false;
        t.handled = false;
        if (cart)
        {
            const auto& hdr = nds.NDSCartSlot.GetCart()->GetHeader();
            setGame(t, hdr.GameCode);
            if (t.supported)
            {
                PC_LOG("%s detected (%s, offset %d), encounter reporting active", t.game->name, t.gameCode, (int)t.offset);
                startLookup(t);
            }
        }
    }
    if (!t.supported && t.queue.empty())
        return;
    processFrame(t, ramRead, &nds);
}
#endif

}
