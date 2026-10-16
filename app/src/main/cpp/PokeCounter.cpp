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

#include <algorithm>
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
    int32_t  partyCountOff;     // anchor + this: u8 party size
    int32_t  partyDataOff;      // anchor + this: 6 x 236-byte party structs
};

constexpr GameLayout kGames[] = {
    {"IPK", "HeartGold",  0x021D4158, 0x6930,  0xC14,   0xC18,   0x021E76D2, -0x23F64, {0, -0x20, 0x20, -0x60, 0x20, -0x3B08}, -0x23F44, -0x23F40},
    {"IPG", "SoulSilver", 0x021D4158, 0x6930,  0xC14,   0xC18,   0x021E76D2, -0x23F64, {0, -0x20, 0x20, -0x60, 0x40, -0x3B08}, -0x23F44, -0x23F40},
    {"CPU", "Platinum",   0x021C0794, 0x217A8, -0x2D5C, -0x2D58, 0x021D18F2, 0x8C,     {0, 0x1A0, 0x1E0, 0x160, 0x200, -0xC00}, 0xB0, 0xB4},
    {"ADA", "Diamond",    0x021C489C, 0x226FE, -0x2B74, -0x2B70, 0x021A1B2A, -0x12,    {0, 0x140, 0x180, 0xE0, 0x1A0, 0x1860}, 0xE, 0x12},
    {"APA", "Pearl",      0x021C489C, 0x226FE, -0x2B74, -0x2B70, 0x021A1B2A, -0x12,    {0, 0x140, 0x180, 0xE0, 0x1A0, 0x1860}, 0xE, 0x12},
};

// Wait this long (frames) for valid foe data after a battle started
constexpr int kFoeTimeoutFrames = 600;
// UDP retry (wall clock, so fast forward does not shorten it). A report is
// repeated until the display confirms it: Wi-Fi drops out for seconds when
// the handheld roams between access points, and a late report counts just
// as well (the display drops repeats by eid). Retries slow down to 2 s.
constexpr int kRetryMs = 400;
constexpr int kRetryMaxMs = 2000;
constexpr size_t kQueueMax = 500;
constexpr uint64_t kAckGapMs = 300;   // hold the next report back after an ack
// Team: checked this often outside battles (frames), sent when a slot changed
constexpr int kTeamCheckFrames = 60;
constexpr int kBoxSlotBase = 100;     // Pending::slot of box packets: 100 + box * 2 + half

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
    int slot = 0;          // team packet for this party slot (1..6), 0 = encounter
    bool sent = false;
};

// ---- egg hunt (Masuda): state, see "Egg hunt" below ------------------------
struct EggLayout
{
    const char* prefix;
    uint32_t trainerX, trainerZ;   // absolute (+ language offset): u32, tile in the upper half
    int32_t mapHeaderOff;          // anchor + this: u16 map id
    int32_t bikeOff;               // anchor + this: u8, 1 = on the bike
    int32_t gearOff;               // anchor + this: u8 bike gear (D/P/Pt), 0 = none (HGSS)
    int32_t daycareEggOff;         // anchor + this: u32, non-zero = the day-care man has an egg
    int16_t pathX, pathZ1, pathZ2; // long path next to the day care: x, both ends (z)
    int16_t pickZ, pickX;          // to the day-care man: first z, then x, then A
    bool hgss;
};

struct Waypoint
{
    char axis;     // 'x' or 'z'
    int16_t value;
};

struct EggHunt
{
    enum Phase { Off, Boot, Settle, Mount, Walk, Pickup, Hatching, AfterHatch };
    Phase phase = Off;
    Phase resume = Walk;        // after a hatch
    const EggLayout* L = nullptr;
    // start point (where the game was saved)
    uint16_t map = 0;
    int16_t x0 = 0, z0 = 0;
    unsigned party0 = 0;
    // progress
    uint32_t frame = 0;         // in the current phase
    uint32_t sinceReset = 0;
    uint32_t sinceProgress = 0; // no egg picked up / hatched
    uint32_t stuck = 0;
    int16_t lastX = 0, lastZ = 0;
    uint32_t nextPress = 0, pressLeft = 0, pressKey = 0;
    int mountTries = 0, bootFails = 0;
    Waypoint route[8];
    int routeLen = 0, routePos = 0, routeSign = 0;
    bool towardZ2 = true;
    bool eggs[6] = {};
    uint16_t sums[6] = {};
    unsigned count = 0;
    int hatchSlot = -1;
    uint16_t hatchSum = 0;
    unsigned pickupCount = 0;
    bool gotEgg = false;
    bool seenBlank = false;     // the reset has happened (the save is not loaded)
    uint32_t keys = 0;          // for the next frame
    uint32_t cycles = 0, hatched = 0;
    int stopReason = 0;
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

    // team (t=mon packets for the display's Team tab)
    std::string lastTeam[6];
    int lastTeamCount = -1;
    uint64_t teamCheckFrame = 0;

    // PC boxes (from the save memory)
    uint64_t boxCheckMs = 0;

    // Set from any thread when a savestate (or rewind state) was loaded
    std::atomic<bool> stateLoaded{false};

    // shiny auto-pause (see PokeCounter.h)
    std::atomic<bool> shinyEvent{false};
    std::atomic<bool> shinyHold{false};
    uint64_t shinyAtMs = 0;   // when the shiny was reported

    // egg hunt (Masuda method)
    EggHunt egg;

    // every reported wild Pokémon (auto-hunt, see AutoHunt.h)
    std::atomic<uint32_t> encounterSeq{0};
    std::atomic<bool> lastShiny{false};

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
            // only an answer to something that went out confirms the head
            if (!t.queue.empty() && t.queue.front().sent && std::strstr(buf, "\"ok\"") != nullptr)
            {
                t.queue.pop_front();
                if (!t.queue.empty())
                    t.queue.front().nextMs = std::max(t.queue.front().nextMs, nowMs() + kAckGapMs);
            }
            fromLen = sizeof(from);
        }
    }

    if (t.queue.empty())
        return;
    Pending& p = t.queue.front();
    const uint64_t now = nowMs();
    if (now < p.nextMs)
        return;
    if (p.tries == 20)
    {
        PC_LOG("no reply from display for a while, still retrying: %s", p.json.c_str());
        t.learnedIp = 0;   // fall back to lookup / broadcast (address may have changed)
    }
    sendDatagram(t, p.json);
    p.sent = true;
    p.tries++;
    p.nextMs = now + std::min<uint64_t>(kRetryMaxMs, (uint64_t)kRetryMs * (1 + p.tries / 6));
}

void report(Tracker& t, const DecodedMon& m)
{
    char json[224];
    const char gender = m.gender == 0 ? 'M' : (m.gender == 1 ? 'F' : 'N');
    std::snprintf(json, sizeof(json),
                  "{\"t\":\"enc\",\"sp\":%u,\"lv\":%u,\"g\":\"%c\",\"shiny\":%s,\"pid\":%u,\"eid\":%u,"
                  "\"ivs\":%u,\"ab\":%u,\"it\":%u,\"tsv\":%u,\"src\":\"%s\"}",
                  m.species, m.level, gender, m.shiny ? "true" : "false", m.pid, (unsigned)randomEid(),
                  (unsigned)m.ivs, m.ability, m.item,
                  // Wild Pokémon carry the player's IDs: TID ^ SID, so the display can show
                  // the shiny value (TID ^ SID ^ PID high ^ PID low, shiny below 8)
                  (unsigned)(m.otId ^ m.otSid), t.gameCode);
    PC_LOG("wild encounter: %s", json);
    t.lastShiny.store(m.shiny);
    t.encounterSeq.fetch_add(1);
    if (m.shiny && autoPauseEnabled())
    {
        t.shinyHold.store(true);
        t.shinyAtMs = nowMs();
        t.shinyEvent.store(true);
        PC_LOG("shiny! pausing this instance, reset and savestate loading blocked until resumed");
    }
    if (t.queue.size() >= kQueueMax)
        t.queue.pop_front();
    Pending p;
    p.json = json;
    p.nextMs = 0;
    // ahead of box packets that have not gone out yet: an encounter (and a
    // shiny pause waiting for it) does not queue behind a whole PC upload
    auto it = t.queue.begin();
    if (it != t.queue.end())
        ++it;
    while (it != t.queue.end() && it->slot < kBoxSlotBase)
        ++it;
    t.queue.insert(it, std::move(p));
}

// ---------------------------------------------------------------------------
// Team: one {"t":"mon",...} packet per party Pokémon for the display's Team
// tab (IVs, EVs, stats, moves ...). Checked outside battles about once a
// second; only slots whose data changed are sent, and a newer state of a
// slot replaces one still waiting in the queue.
// ---------------------------------------------------------------------------

// Gen 4 character codes 0x0121.. (Latin part), from pokebot-nds (MIT)
constexpr uint16_t kCharFirst = 0x0121;
const char* const kChars[] = {
    "0", "1", "2", "3", "4", "5", "6", "7", "8", "9", "A", "B",
    "C", "D", "E", "F", "G", "H", "I", "J", "K", "L", "M", "N",
    "O", "P", "Q", "R", "S", "T", "U", "V", "W", "X", "Y", "Z",
    "a", "b", "c", "d", "e", "f", "g", "h", "i", "j", "k", "l",
    "m", "n", "o", "p", "q", "r", "s", "t", "u", "v", "w", "x",
    "y", "z", "À", "Á", "Â", "Ã", "Ä", "Å", "Æ", "Ç", "È", "É",
    "Ê", "Ë", "Ì", "Í", "Î", "Ï", "Ð", "Ñ", "Ò", "Ó", "Ô", "Õ",
    "Ö", "×", "Ø", "Ù", "Ú", "Û", "Ü", "Ý", "Þ", "ß", "à", "á",
    "â", "ã", "ä", "å", "æ", "ç", "è", "é", "ê", "ë", "ì", "í",
    "î", "ï", "ð", "ñ", "ò", "ó", "ô", "õ", "ö", "÷", "ø", "ù",
    "ú", "û", "ü", "ý", "þ", "ÿ", "Œ", "œ", "Ş", "ş", "ª", "º",
    "er", "re", "r", "₽", "¡", "¿", "!", "?", ",", ".", "…", "･",
    "/", "‘", "’", "“", "”", "„", "«", "»", "(", ")", "♂", "♀",
    "+", "-", "*", "#", "=", "&", "~", ":", ";", "♠", "♣", "♥",
    "♦", "★", "◎", "○", "□", "△", "◇", "@", "♪", "%", "☀", "☁",
    "☂", "☃", "😑", "☺", "☹", "😠", "⤴︎", "⤵︎", "💤", " ", "e", "PK",
    "MN", " ", " ", " ", "", " ", " ", "°", "_", "＿", "․", "‥",
};

// Decrypts the 128 data bytes (blocks A..D in order, offsets 0x08..0x87) and
// the first 20 bytes of the party stats (0x88..0x9B).
bool decryptMon(ReadFn read, void* ctx, uint32_t addr, uint8_t data[128], uint8_t stats[20], bool raw)
{
    const uint32_t pid = rd32(read, ctx, addr);
    const uint16_t checksum = rd16(read, ctx, addr + 6);
    if (checksum == 0)
        return false;
    uint8_t stored[4][32];
    uint32_t seed = checksum;
    for (int b = 0; b < 4; b++)
        for (int i = 0; i < 32; i += 2)
        {
            seed = lcrng(seed);
            uint16_t w = rd16(read, ctx, addr + 0x08 + b * 32 + i) ^ (raw ? 0 : (uint16_t)(seed >> 16));
            stored[b][i] = (uint8_t)w;
            stored[b][i + 1] = (uint8_t)(w >> 8);
        }
    const uint8_t* order = kBlockOrder[((pid & 0x3E000u) >> 13) % 24];
    for (int b = 0; b < 4; b++)
        std::memcpy(data + b * 32, stored[order[b] - 1], 32);
    uint16_t sum = 0;
    for (int i = 0; i < 128; i += 2)
        sum = (uint16_t)(sum + (data[i] | (data[i + 1] << 8)));
    if (sum != checksum)
        return false;
    seed = pid;
    for (int i = 0; i < 20; i += 2)
    {
        seed = lcrng(seed);
        uint16_t w = rd16(read, ctx, addr + 0x88 + i) ^ (raw ? 0 : (uint16_t)(seed >> 16));
        stats[i] = (uint8_t)w;
        stats[i + 1] = (uint8_t)(w >> 8);
    }
    return true;
}

void jsonEscape(std::string& out, const std::string& s)
{
    out += '"';
    for (unsigned char c : s)
    {
        if (c == '"' || c == '\\') { out += '\\'; out += (char)c; }
        else if (c < 0x20) { char b[8]; std::snprintf(b, sizeof(b), "\\u%04x", c); out += b; }
        else out += (char)c;
    }
    out += '"';
}

std::string decodeName(const uint8_t* p, int maxChars)
{
    std::string s;
    for (int i = 0; i < maxChars; i++)
    {
        uint16_t c = (uint16_t)(p[i * 2] | (p[i * 2 + 1] << 8));
        if (c == 0xFFFF || c == 0)
            break;
        if (c >= kCharFirst && c < kCharFirst + sizeof(kChars) / sizeof(kChars[0]))
            s += kChars[c - kCharFirst];
        else if (c == 0x01DE)
            s += ' ';
        else
            s += '?';
    }
    return s;
}

// JSON for one party slot, or "" if the data is not valid (being written)
std::string teamMonJson(Tracker& t, ReadFn read, void* ctx, uint32_t addr, int slot, int count)
{
    uint8_t data[128], st[20];
    bool raw = false;
    if (!decryptMon(read, ctx, addr, data, st, false))
    {
        if (!decryptMon(read, ctx, addr, data, st, true))
            return "";
        raw = true;
    }
    (void)raw;
    auto u8 = [&](int off) { return (unsigned)data[off - 0x08]; };
    auto u16 = [&](int off) { return (unsigned)(data[off - 0x08] | (data[off - 0x07] << 8)); };
    auto u32 = [&](int off) { return (uint32_t)u16(off) | ((uint32_t)u16(off + 2) << 16); };
    auto s16 = [&](int off) { return (unsigned)(st[off - 0x88] | (st[off - 0x87] << 8)); };
    const uint32_t pid = rd32(read, ctx, addr);
    const unsigned species = u16(0x08);
    if (species < 1 || species > 493)
        return "";
    const uint32_t ivWord = u32(0x38);
    char head[96];
    if (ivWord & 0x40000000u)   // egg
    {
        std::snprintf(head, sizeof(head), "{\"t\":\"mon\",\"src\":\"%s\",\"slot\":%d,\"n\":%d,\"egg\":true}", t.gameCode, slot, count);
        return head;
    }
    const unsigned level = st[0x8C - 0x88];
    if (level < 1 || level > 100)
        return "";
    const unsigned tid = u16(0x0C), sid = u16(0x0E);
    const bool shiny = (uint16_t)(tid ^ sid ^ (pid >> 16) ^ (pid & 0xFFFF)) < 8;
    const unsigned g = (u8(0x40) >> 1) & 3;
    // wire order of the six values: HP, Atk, Def, SpA, SpD, Spe
    const unsigned ivs[6] = {ivWord & 31, (ivWord >> 5) & 31, (ivWord >> 10) & 31,
                             (ivWord >> 20) & 31, (ivWord >> 25) & 31, (ivWord >> 15) & 31};
    const unsigned evs[6] = {u8(0x18), u8(0x19), u8(0x1A), u8(0x1C), u8(0x1D), u8(0x1B)};
    const unsigned stats[6] = {s16(0x90), s16(0x92), s16(0x94), s16(0x98), s16(0x9A), s16(0x96)};
    const uint32_t status = (uint32_t)(st[0] | (st[1] << 8) | (st[2] << 16) | ((uint32_t)st[3] << 24));
    const char* statusName = (status & 7) ? "SCHLAF" : (status & 0x88) ? "GIFT" : (status & 0x10) ? "BRAND"
                           : (status & 0x20) ? "EIS" : (status & 0x40) ? "PAR" : "";
    std::string j;
    j.reserve(512);
    char b[160];
    std::snprintf(b, sizeof(b), "{\"t\":\"mon\",\"src\":\"%s\",\"slot\":%d,\"n\":%d,\"pid\":%u,\"sp\":%u,\"lv\":%u,\"nick\":",
                  t.gameCode, slot, count, (unsigned)pid, species, level);
    j += b;
    jsonEscape(j, decodeName(&data[0x48 - 0x08], 11));
    std::snprintf(b, sizeof(b), ",\"g\":\"%c\",\"shiny\":%s,\"nat\":%u,\"ab\":%u,\"it\":%u",
                  g == 0 ? 'M' : g == 1 ? 'F' : 'N', shiny ? "true" : "false", (unsigned)(pid % 25), u8(0x15), u16(0x0A));
    j += b;
    auto arr = [&](const char* key, const unsigned* v, int n) {
        j += ",\"";
        j += key;
        j += "\":[";
        for (int i = 0; i < n; i++)
        {
            if (i) j += ',';
            j += std::to_string(v[i]);
        }
        j += ']';
    };
    arr("ivs", ivs, 6);
    arr("evs", evs, 6);
    arr("st", stats, 6);
    std::snprintf(b, sizeof(b), ",\"hp\":%u,\"stat\":\"%s\"", s16(0x8E), statusName);
    j += b;
    const unsigned mv[4] = {u16(0x28), u16(0x2A), u16(0x2C), u16(0x2E)};
    const unsigned pp[4] = {u8(0x30), u8(0x31), u8(0x32), u8(0x33)};
    const unsigned ppu[4] = {u8(0x34), u8(0x35), u8(0x36), u8(0x37)};
    arr("mv", mv, 4);
    arr("pp", pp, 4);
    arr("ppu", ppu, 4);
    std::snprintf(b, sizeof(b), ",\"fr\":%u,\"pkrs\":%u,\"exp\":%u,\"ball\":%u}",
                  u8(0x14), u8(0x82), (unsigned)u32(0x10), u8(0x83));
    j += b;
    return j;
}

void enqueueTeam(Tracker& t, std::string json, int slot)
{
    // a newer state replaces one of the same slot that has not gone out yet
    for (auto it = t.queue.begin(); it != t.queue.end();)
    {
        if (it != t.queue.begin() && it->slot == slot && !it->sent)
            it = t.queue.erase(it);
        else
            ++it;
    }
    if (t.queue.size() >= kQueueMax)
        t.queue.pop_front();
    Pending p;
    p.json = std::move(json);
    p.slot = slot;
    t.queue.push_back(std::move(p));
}

void teamTick(Tracker& t, ReadFn read, void* ctx)
{
    if (!teamEnabled() || t.frame < t.teamCheckFrame)
        return;
    t.teamCheckFrame = t.frame + kTeamCheckFrames;
    const GameLayout& L = *t.game;
    const uint32_t anchor = rd32(read, ctx, L.anchorPtr + t.offset);
    if (!isMainRam(anchor))
        return;
    const unsigned count = read(ctx, anchor + L.partyCountOff);
    if (count < 1 || count > 6)
        return;
    std::string fresh[6];
    for (unsigned i = 0; i < count; i++)
    {
        fresh[i] = teamMonJson(t, read, ctx, anchor + L.partyDataOff + i * kMonSize, (int)i + 1, (int)count);
        if (fresh[i].empty())
            return;   // being written (e.g. right after a swap), next time
    }
    const bool sizeChanged = (int)count != t.lastTeamCount;
    for (unsigned i = 0; i < count; i++)
    {
        if (sizeChanged || fresh[i] != t.lastTeam[i])
        {
            t.lastTeam[i] = fresh[i];
            enqueueTeam(t, fresh[i], (int)i + 1);
        }
    }
    for (unsigned i = count; i < 6; i++)
        t.lastTeam[i].clear();
    t.lastTeamCount = (int)count;
}


// ---------------------------------------------------------------------------
// PC boxes: read from the save memory (the state of the last in-game save),
// sent as {"t":"box",...} packets, half a box each (one UDP datagram). Checked
// every 2 s; only boxes whose bytes changed are sent. Instances that run the
// same game share what was sent, so 10 hunting windows send it once.
// Save layout from PKHeX (SAV4Sinnoh / SAV4HGSS): two 0x40000 partitions, the
// storage block of each with a footer counter; the newer copy is the current one.
// ---------------------------------------------------------------------------

constexpr int kBoxCount = 18;
constexpr int kBoxMonSize = 136;
constexpr uint64_t kBoxCheckMs = 2000;

struct SaveLayout
{
    const char* prefix;
    uint32_t storageStart, storageSize;   // in each partition
    uint32_t boxStride;                   // box i at boxFirst + i * boxStride
    uint32_t boxFirst;
    uint32_t names;                       // 18 x 40 bytes (20 characters)
    uint32_t current;                     // u8 current box (0-based)
};

constexpr SaveLayout kSaves[] = {
    {"IPK", 0xF700, 0x12310, 0x1000, 0, 0x12008, 0x12000},
    {"IPG", 0xF700, 0x12310, 0x1000, 0, 0x12008, 0x12000},
    {"CPU", 0xCF2C, 0x121E4, 0xFF0, 4, 0x11EE4, 0},
    {"ADA", 0xC100, 0x121E0, 0xFF0, 4, 0x11EE4, 0},
    {"APA", 0xC100, 0x121E0, 0xFF0, 4, 0x11EE4, 0},
};

// Growth rate per species (1 slow, 2 medium, 3 fast, 4 medium slow,
// 5 erratic, 6 fluctuating; PokeAPI pokemon_species.csv)
const char kGrowth[] =
    "4444444442222224442222222222444444332233224442222222222221144444444444"
    "4114442222222222222114442222222112222222113222222112222211111222222222"
    "2111111111444444444422223333211233332244443324444344222224232222224233"
    "2241422221132231111222213222221311111111444444444422222222244444444221"
    "1122661115554446632334311122112256466116622233344444555633226622555555"
    "2433331342224445551311111111111111114444444444442244444445555222442222"
    "2223662234333222222342111144111122155111422122232224121232211111111111"
    "141";

uint64_t expForLevel(int rate, uint64_t x)
{
    if (x <= 1)
        return 0;
    const uint64_t c = x * x * x;
    switch (rate)
    {
    case 1: return 5 * c / 4;
    case 2: return c;
    case 3: return 4 * c / 5;
    case 4: return 6 * c / 5 + 100 * x - 15 * x * x - 140;
    case 5:
        if (x <= 50) return c * (100 - x) / 50;
        if (x <= 68) return c * (150 - x) / 100;
        if (x <= 98) return c * ((1911 - 10 * x) / 3) / 500;
        return c * (160 - x) / 100;
    default:
        if (x <= 15) return c * ((x + 1) / 3 + 24) / 50;
        if (x <= 36) return c * (x + 14) / 50;
        return c * (x / 2 + 32) / 50;
    }
}

} // namespace

int levelFromExp(int species, uint32_t exp)
{
    if (species < 1 || species > 493)
        return 0;
    const int rate = kGrowth[species - 1] - '0';
    int lv = 1;
    while (lv < 100 && expForLevel(rate, (uint64_t)lv + 1) <= exp)
        lv++;
    return lv;
}

namespace
{

struct SaveBuf
{
    const uint8_t* p;
    uint32_t len;
};

uint8_t saveRead(void* ctx, uint32_t addr)
{
    auto* s = static_cast<SaveBuf*>(ctx);
    return addr < s->len ? s->p[addr] : 0;
}

uint32_t le32(const uint8_t* p) { return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24); }

// PKHeX SAV4BlockDetection.CompareCounters: 0 first, 1 second, 2 same
int compareCounters(uint32_t c1, uint32_t c2)
{
    if (c1 == 0xFFFFFFFFu && c2 != 0xFFFFFFFEu) return 1;
    if (c2 == 0xFFFFFFFFu && c1 != 0xFFFFFFFEu) return 0;
    if (c1 > c2) return 0;
    if (c1 < c2) return 1;
    return 2;
}

std::mutex gBoxLock;
std::map<std::string, uint64_t> gBoxSent;   // "IPGD:3" -> hash of what was sent

uint64_t fnv(const uint8_t* p, size_t n, uint64_t h = 1469598103934665603ull)
{
    for (size_t i = 0; i < n; i++)
        h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// One box Pokémon as [slot,sp,lv,fl,pid,ivs,it,ab,"nick",ball], "" if empty or invalid
std::string boxMonJson(const uint8_t* mon, int slot)
{
    SaveBuf sb{mon, kBoxMonSize};
    uint8_t data[128], st[20];
    if (le32(mon) == 0 && mon[6] == 0 && mon[7] == 0)
        return "";
    if (!decryptMon(saveRead, &sb, 0, data, st, false))
        return "";
    auto u8 = [&](int off) { return (unsigned)data[off - 0x08]; };
    auto u16 = [&](int off) { return (unsigned)(data[off - 0x08] | (data[off - 0x07] << 8)); };
    auto u32 = [&](int off) { return (uint32_t)u16(off) | ((uint32_t)u16(off + 2) << 16); };
    const uint32_t pid = le32(mon);
    const unsigned species = u16(0x08);
    if (species < 1 || species > 493)
        return "";
    const uint32_t ivWord = u32(0x38);
    const bool egg = ivWord & 0x40000000u, nicked = ivWord & 0x80000000u;
    const unsigned tid = u16(0x0C), sid = u16(0x0E);
    const bool shiny = (uint16_t)(tid ^ sid ^ (pid >> 16) ^ (pid & 0xFFFF)) < 8;
    const unsigned g = (u8(0x40) >> 1) & 3;
    const unsigned fl = (shiny ? 1u : 0u) | ((g > 2 ? 2 : g) << 1) | (egg ? 8u : 0u);
    const int lv = egg ? 0 : levelFromExp((int)species, u32(0x10));
    std::string j;
    char b[128];
    std::snprintf(b, sizeof(b), "[%d,%u,%d,%u,%u,%u,%u,%u,", slot, species, lv, fl, (unsigned)pid,
                  (unsigned)(ivWord & 0x3FFFFFFFu), u16(0x0A), u8(0x15));
    j += b;
    jsonEscape(j, nicked && !egg ? decodeName(&data[0x48 - 0x08], 11) : std::string());
    std::snprintf(b, sizeof(b), ",%u]", u8(0x83));
    j += b;
    return j;
}

} // namespace

void processSave(Tracker& t, const uint8_t* save, uint32_t len)
{
    if (!t.supported || !save)
        return;
    const SaveLayout* S = nullptr;
    for (const SaveLayout& L : kSaves)
        if (std::strncmp(t.gameCode, L.prefix, 3) == 0)
            S = &L;
    if (!S || len < 0x80000)
        return;
    const uint64_t now = nowMs();
    if (now < t.boxCheckMs)
        return;
    t.boxCheckMs = now + kBoxCheckMs;

    const uint32_t foot = S->storageStart + S->storageSize - 0x14;
    uint32_t base = S->storageStart;
    const uint32_t a = le32(save + foot), b = le32(save + foot + 0x40000);
    if (a == 0xFFFFFFFFu && b == 0xFFFFFFFFu)
        return;   // never saved
    int newer = compareCounters(a, b);
    if (newer == 2)
        newer = compareCounters(le32(save + foot + 4), le32(save + foot + 0x40004)) == 1 ? 1 : 0;
    if (newer == 1)
        base += 0x40000;
    const uint8_t* st = save + base;
    unsigned cur = st[S->current];
    if (S->current == 0 && S->boxFirst == 4)
        cur = le32(st);
    cur = cur < kBoxCount ? cur + 1 : 1;

    for (int box = 0; box < kBoxCount; box++)
    {
        const uint8_t* bx = st + S->boxFirst + box * S->boxStride;
        const uint8_t* nameP = st + S->names + box * 40;
        uint64_t h = fnv(bx, 30 * kBoxMonSize);
        h = fnv(nameP, 40, h);
        h = fnv(reinterpret_cast<const uint8_t*>(&cur), sizeof(cur), h);
        const std::string key = std::string(t.gameCode) + ":" + std::to_string(box);
        {
            std::lock_guard<std::mutex> lk(gBoxLock);
            auto it = gBoxSent.find(key);
            if (it != gBoxSent.end() && it->second == h)
                continue;
            gBoxSent[key] = h;
        }
        std::string name = decodeName(nameP, 20);
        if (name.empty())
            name = "BOX " + std::to_string(box + 1);
        for (int half = 0; half < 2; half++)
        {
            std::string j;
            j.reserve(1200);
            char hd[128];
            std::snprintf(hd, sizeof(hd), "{\"t\":\"box\",\"src\":\"%s\",\"b\":%d,\"h\":%d,\"nb\":%d,\"cur\":%u,\"name\":",
                          t.gameCode, box + 1, half, kBoxCount, cur);
            j += hd;
            jsonEscape(j, name);
            j += ",\"m\":[";
            bool first = true;
            for (int s = half * 15; s < half * 15 + 15; s++)
            {
                std::string m = boxMonJson(bx + s * kBoxMonSize, s + 1);
                if (m.empty())
                    continue;
                if (!first) j += ',';
                first = false;
                j += m;
            }
            j += "]}";
            enqueueTeam(t, std::move(j), kBoxSlotBase + box * 2 + half);
        }
        PC_LOG("box %d (%s) changed, sending", box + 1, name.c_str());
    }
}

namespace
{
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

// ---------------------------------------------------------------------------
// Egg hunt (Masuda method), Gen 4. The player saves on the long path next to
// the day care (HGSS: Route 34, D/P/Pt: Solaceon Town) with the parents in
// the day care, free slots in the party and the bike on Y. Per window,
// toggled by a hotkey:
//   reset -> A through title and "Continue" -> bike up and down the path ->
//   when the day-care man has an egg: ride to him and take it -> check it
//   right away: in Gen 4 the PID (rolled up to 5 times with Masuda) is fixed
//   when the egg is made, and the egg hatches shiny if PID and the player's
//   TID/SID give a shiny value below 8 -> every egg is reported like an
//   encounter -> shiny egg: stop (the shiny auto-pause holds the window, the
//   player hatches it), party full: reset and start over. No hatching needed.
//   Should an egg hatch on the way anyway, its texts are clicked through.
// Coordinates, pointers and routes from pokebot-nds (methods/hgss.lua,
// gen_iv.lua, pt.lua; MIT).
// ---------------------------------------------------------------------------

namespace
{

constexpr EggLayout kEggGames[] = {
    {"IPK", 0x21DA6F4, 0x21DA6FC, -0x22DA4, -0x22D34, 0, -0x22804, 358, 380, 410, 410, 364, true},
    {"IPG", 0x21DA6F4, 0x21DA6FC, -0x22DA4, -0x22D34, 0, -0x22804, 358, 380, 410, 410, 364, true},
    {"CPU", 0x21C5CE4, 0x21C5CEC, 0x1294, 0x1324, 0x1320, 0x1840, 562, 630, 675, 648, 556, false},
    {"ADA", 0x21CEF70, 0x21CEF78, 0x11B2, 0x1242, 0x123E, 0x156E, 562, 630, 675, 648, 556, false},
    {"APA", 0x21CEF70, 0x21CEF78, 0x11B2, 0x1242, 0x123E, 0x156E, 562, 630, 675, 648, 556, false},
};

constexpr uint32_t kKeyA = 1u << 0, kKeyB = 1u << 1, kKeyRight = 1u << 4, kKeyLeft = 1u << 5,
                   kKeyUp = 1u << 6, kKeyDown = 1u << 7, kKeyY = 1u << 11;
constexpr uint32_t kEggBootMax = 4000;          // frames from reset to the overworld
constexpr uint32_t kEggHatchMax = 2400;         // hatch animation and texts
constexpr uint32_t kEggPickupMax = 2400;        // talking to the day-care man
constexpr uint32_t kEggStuckMax = 1200;         // not moving while holding a direction
constexpr uint32_t kEggNoProgressMax = 60 * 60 * 15;   // 15 game minutes without an egg or a hatch

struct EggRam
{
    uint16_t map = 0;
    int16_t x = 0, z = 0;
    unsigned count = 0;
    bool eggs[6] = {};
    uint16_t sums[6] = {};
    bool bike = false;
    uint8_t gear = 1;
    bool daycareEgg = false;
    uint32_t anchor = 0;
};

uint32_t eggRand(uint32_t lo, uint32_t hi)
{
    std::lock_guard<std::mutex> lk(gRngLock);
    return lo + (uint32_t)(gRng() % (hi - lo + 1));
}

const EggLayout* eggLayoutFor(const Tracker& t)
{
    for (const EggLayout& L : kEggGames)
        if (std::strncmp(t.gameCode, L.prefix, 3) == 0)
            return &L;
    return nullptr;
}

bool readEggRam(const Tracker& t, ReadFn read, void* ctx, EggRam& r)
{
    const GameLayout& G = *t.game;
    const EggLayout& L = *t.egg.L;
    r = EggRam();
    const uint32_t anchor = rd32(read, ctx, G.anchorPtr + t.offset);
    if (!isMainRam(anchor))
        return false;
    r.anchor = anchor;
    r.map = rd16(read, ctx, anchor + L.mapHeaderOff);
    r.x = (int16_t)(rd32(read, ctx, L.trainerX + t.offset) >> 16);
    r.z = (int16_t)(rd32(read, ctx, L.trainerZ + t.offset) >> 16);
    r.count = read(ctx, anchor + G.partyCountOff);
    if (r.count < 1 || r.count > 6)
        return false;
    for (unsigned i = 0; i < r.count; i++)
    {
        const uint32_t addr = anchor + G.partyDataOff + i * kMonSize;
        uint8_t data[128], st[20];
        if (!decryptMon(read, ctx, addr, data, st, false))
            return false;   // being written (hatch, new egg)
        const uint32_t iv = (uint32_t)data[0x30] | ((uint32_t)data[0x31] << 8) | ((uint32_t)data[0x32] << 16) | ((uint32_t)data[0x33] << 24);
        r.eggs[i] = (iv & 0x40000000u) != 0;
        r.sums[i] = rd16(read, ctx, addr + 6);
    }
    r.bike = read(ctx, anchor + L.bikeOff) == 1;
    r.gear = L.gearOff ? read(ctx, anchor + L.gearOff) : 1;
    r.daycareEgg = rd32(read, ctx, anchor + L.daycareEggOff) != 0;
    return true;
}

// The egg in party slot `addr` as an encounter: species, PID, IVs ... and
// whether it hatches shiny for this player (Gen 4 decides with the hatcher's IDs)
bool eggMon(const Tracker& t, ReadFn read, void* ctx, uint32_t anchor, uint32_t addr, DecodedMon& m)
{
    uint8_t data[128], st[20];
    if (!decryptMon(read, ctx, addr, data, st, false))
        return false;
    auto u8 = [&](int off) { return (unsigned)data[off - 0x08]; };
    auto u16 = [&](int off) { return (unsigned)(data[off - 0x08] | (data[off - 0x07] << 8)); };
    m = DecodedMon();
    m.pid = rd32(read, ctx, addr);
    m.species = (uint16_t)u16(0x08);
    if (m.species < 1 || m.species > 493)
        return false;
    m.otId = rd16(read, ctx, anchor + t.game->trainerIdOff);
    m.otSid = rd16(read, ctx, anchor + t.game->trainerIdOff + 2);
    m.level = 1;
    m.gender = (uint8_t)((u8(0x40) >> 1) & 3);
    m.ivs = ((uint32_t)u16(0x38) | ((uint32_t)u16(0x3A) << 16)) & 0x3FFFFFFFu;
    m.ability = (uint8_t)u8(0x15);
    m.item = 0;
    m.shiny = (uint16_t)(m.otId ^ m.otSid ^ (m.pid >> 16) ^ (m.pid & 0xFFFF)) < 8;
    return true;
}

// Short presses of `key` at random gaps (like a person, gives every run its own timing)
uint32_t eggPress(EggHunt& e, uint32_t key, uint32_t gapMin = 8, uint32_t gapMax = 30)
{
    if (e.pressLeft == 0 && e.frame >= e.nextPress)
    {
        e.pressLeft = 3;
        e.pressKey = key;
        e.nextPress = e.frame + 3 + eggRand(gapMin, gapMax);
    }
    if (e.pressLeft == 0)
        return 0;
    e.pressLeft--;
    return e.pressKey;
}

void eggPhase(EggHunt& e, EggHunt::Phase p)
{
    e.phase = p;
    e.frame = 0;
    e.pressLeft = 0;
    e.nextPress = 0;
    e.stuck = 0;
}

void eggRoute(EggHunt& e, std::initializer_list<Waypoint> w)
{
    e.routeLen = 0;
    for (const Waypoint& p : w)
        e.route[e.routeLen++] = p;
    e.routePos = 0;
    e.routeSign = 0;
}

// Direction toward the current waypoint, or 0 once it is reached (or passed:
// the bike can run one tile further after letting go).
uint32_t eggSteer(EggHunt& e, const EggRam& r, bool& done)
{
    done = false;
    while (e.routePos < e.routeLen)
    {
        const Waypoint& w = e.route[e.routePos];
        const int cur = w.axis == 'x' ? r.x : r.z;
        const int diff = w.value - cur;
        if (e.routeSign == 0)
            e.routeSign = diff > 0 ? 1 : (diff < 0 ? -1 : 0);
        if (diff == 0 || (e.routeSign != 0 && diff * e.routeSign < 0))
        {
            e.routePos++;
            e.routeSign = 0;
            continue;
        }
        if (w.axis == 'x')
            return diff > 0 ? kKeyRight : kKeyLeft;
        return diff > 0 ? kKeyDown : kKeyUp;
    }
    done = true;
    return 0;
}

EggStep eggStopWith(EggHunt& e, EggStop why)
{
    e.stopReason = (int)why;
    e.phase = EggHunt::Off;
    e.keys = 0;
    return EggStep::Stopped;
}

EggStep eggResetWith(EggHunt& e)
{
    eggPhase(e, EggHunt::Boot);
    e.sinceReset = 0;
    e.seenBlank = false;
    e.mountTries = 0;
    e.count = 0;
    e.keys = 0;
    e.nextPress = eggRand(0, 240);   // random start delay: every reset its own RNG seed
    return EggStep::Reset;
}

} // namespace

const char* eggStart(Tracker& t, ReadFn read, void* ctx)
{
    EggHunt& e = t.egg;
    if (!t.supported || !t.game)
        return "Egg hunt: only for Pokemon Diamond, Pearl, Platinum, HeartGold, SoulSilver";
    e.L = eggLayoutFor(t);
    if (!e.L)
        return "Egg hunt: game not supported";
    EggRam r;
    if (!readEggRam(t, read, ctx, r))
        return "Egg hunt: load your save first (standing on the path next to the day care)";
    if (r.count >= 6)
        return "Egg hunt: the party is full - leave room for the eggs";
    e.map = r.map;
    e.x0 = r.x;
    e.z0 = r.z;
    e.party0 = r.count;
    e.cycles = 0;
    e.hatched = 0;
    e.bootFails = 0;
    e.sinceProgress = 0;
    e.stopReason = 0;
    PC_LOG("egg hunt: start map %u at %d,%d, party %u", r.map, r.x, r.z, r.count);
    eggResetWith(e);
    return nullptr;
}

void eggStop(Tracker& t)
{
    eggStopWith(t.egg, EggStop::ByUser);
}

bool eggActive(const Tracker& t) { return t.egg.phase != EggHunt::Off; }
uint32_t eggKeys(const Tracker& t) { return t.egg.phase != EggHunt::Off ? t.egg.keys : 0; }
EggStop eggStopReason(const Tracker& t) { return (EggStop)t.egg.stopReason; }
uint32_t eggCycles(const Tracker& t) { return t.egg.cycles; }
uint32_t eggHatched(const Tracker& t) { return t.egg.hatched; }

EggStep eggAfterFrame(Tracker& t, ReadFn read, void* ctx)
{
    EggHunt& e = t.egg;
    if (e.phase == EggHunt::Off)
        return EggStep::None;
    e.frame++;
    e.sinceProgress++;
    e.keys = 0;

    EggRam r;
    const bool ok = readEggRam(t, read, ctx, r);

    if (e.phase == EggHunt::Boot)
    {
        // the frontend may reset a little later (Android: from another
        // thread): nothing happens until the save is gone
        if (!e.seenBlank)
        {
            e.seenBlank = !ok;
            if (!e.seenBlank && ++e.sinceReset > 600)
                return eggResetWith(e);   // the reset got lost: ask again
            if (e.seenBlank)
                e.sinceReset = 0;
            return EggStep::None;
        }
        e.sinceReset++;
        if (ok && r.map == e.map && r.x == e.x0 && r.z == e.z0 && r.count == e.party0)
        {
            eggPhase(e, EggHunt::Settle);
            return EggStep::None;
        }
        if (e.sinceReset > kEggBootMax)
        {
            if (++e.bootFails >= 3)
                return eggStopWith(e, EggStop::NotInGame);
            return eggResetWith(e);
        }
        e.keys = eggPress(e, kKeyA);
        return EggStep::None;
    }
    if (!ok)
    {
        // party data being rewritten (egg received, hatch starting): keep
        // clicking through a hatch, otherwise wait a frame
        if (e.phase == EggHunt::Hatching)
            e.keys = eggPress(e, kKeyB, 4, 10);
        return EggStep::None;
    }
    e.bootFails = 0;

    // hatch started: an egg slot is no egg any more
    if (e.phase != EggHunt::Hatching && e.phase != EggHunt::AfterHatch && e.phase != EggHunt::Settle)
    {
        for (unsigned i = 0; i < r.count && i < e.count; i++)
        {
            if (e.eggs[i] && !r.eggs[i])
            {
                eggPhase(e, EggHunt::Hatching);
                e.hatchSlot = (int)i;
                e.hatchSum = r.sums[i];
                break;
            }
        }
    }

    switch (e.phase)
    {
    case EggHunt::Settle:
        // the save is loaded; Platinum shows its journal first (B closes it,
        // and does nothing on the overworld)
        if (e.frame == 40)
            e.keys = kKeyB;
        if (e.frame >= 90)
        {
            eggPhase(e, EggHunt::Mount);
            e.mountTries = 0;
        }
        break;

    case EggHunt::Mount:
        if (!r.bike)
        {
            if (e.frame % 70 == 1)
            {
                if (++e.mountTries > 4)
                    return eggStopWith(e, EggStop::NoBike);
                e.keys = kKeyY;
            }
            break;
        }
        if (e.L->gearOff && r.gear != 1)
        {
            if (e.frame % 40 == 1)
            {
                if (++e.mountTries > 8)
                    return eggStopWith(e, EggStop::NoBike);
                e.keys = kKeyB;   // fast gear
            }
            break;
        }
        eggPhase(e, EggHunt::Walk);
        eggRoute(e, {{'x', e.L->pathX}, {'z', e.towardZ2 ? e.L->pathZ2 : e.L->pathZ1}});
        break;

    case EggHunt::Walk:
    {
        // round done: party full, every egg checked
        if (r.count >= 6)
        {
            e.cycles++;
            PC_LOG("egg hunt: round %u done, no shiny - reset", (unsigned)e.cycles);
            for (unsigned i = 0; i < 6; i++) e.eggs[i] = false;
            return eggResetWith(e);
        }
        if (!r.bike)
        {
            eggPhase(e, EggHunt::Mount);
            break;
        }
        bool done;
        e.keys = eggSteer(e, r, done);
        if (done)
        {
            // at an end of the path: an egg waiting and room for it -> fetch it
            if (r.daycareEgg && r.count < 6)
            {
                eggPhase(e, EggHunt::Pickup);
                eggRoute(e, {{'z', e.L->pickZ}, {'x', e.L->pickX}});
                e.pickupCount = r.count;
                e.gotEgg = false;
                break;
            }
            e.towardZ2 = !e.towardZ2;
            eggRoute(e, {{'z', e.towardZ2 ? e.L->pathZ2 : e.L->pathZ1}});
            e.keys = eggSteer(e, r, done);
        }
        break;
    }

    case EggHunt::Pickup:
    {
        if (!e.gotEgg && r.count > e.pickupCount)
        {
            e.gotEgg = true;
            e.sinceProgress = 0;
            e.frame = 0;
            e.nextPress = 0;
            e.pressLeft = 0;
            DecodedMon m;
            const uint32_t addr = r.anchor + t.game->partyDataOff + (r.count - 1) * kMonSize;
            if (eggMon(t, read, ctx, r.anchor, addr, m))
            {
                e.hatched++;
                PC_LOG("egg hunt: egg #%u (species %u, PID %08X)%s", (unsigned)e.hatched, m.species, (unsigned)m.pid,
                       m.shiny ? " SHINY" : "");
                report(t, m);
                if (m.shiny)
                    return eggStopWith(e, EggStop::Shiny);
            }
            else
            {
                PC_LOG("egg hunt: egg received, data not readable");
            }
        }
        if (e.gotEgg)
        {
            // close the texts, then back to the path
            if (e.frame < 90)
            {
                e.keys = eggPress(e, kKeyB, 6, 14);
                break;
            }
            eggPhase(e, EggHunt::Walk);
            eggRoute(e, {{'x', e.L->pathX}, {'z', e.towardZ2 ? e.L->pathZ2 : e.L->pathZ1}});
            break;
        }
        if (e.frame > kEggPickupMax)
            return eggStopWith(e, EggStop::Blocked);
        bool done;
        e.keys = eggSteer(e, r, done);
        if (done)
            e.keys = eggPress(e, kKeyA);   // "yes, I'll take the egg"
        break;
    }

    case EggHunt::Hatching:
    {
        const int s = e.hatchSlot;
        const bool finished = s >= 0 && (unsigned)s < r.count && r.sums[s] != e.hatchSum && !r.eggs[s];
        if (finished || e.frame > kEggHatchMax)
        {
            // already checked (and counted) when it was picked up
            PC_LOG("egg hunt: an egg hatched on the way");
            eggPhase(e, EggHunt::AfterHatch);
            break;
        }
        e.keys = eggPress(e, kKeyB, 4, 10);
        break;
    }

    case EggHunt::AfterHatch:
        // the "nickname?" question and the last texts: B (= no)
        if (e.frame < 150)
        {
            e.keys = eggPress(e, kKeyB, 6, 14);
            break;
        }
        eggPhase(e, EggHunt::Mount);
        e.mountTries = 0;
        break;

    default:
        break;
    }

    // stuck (a phone call, a text box, something in the way)
    if (e.keys & (kKeyUp | kKeyDown | kKeyLeft | kKeyRight))
    {
        if (r.x == e.lastX && r.z == e.lastZ)
            e.stuck++;
        else
            e.stuck = 0;
        if (e.stuck > kEggStuckMax)
            return eggStopWith(e, EggStop::Blocked);
        if (e.stuck > 90 && e.stuck % 45 < 3)
            e.keys = e.L->hgss ? kKeyB : kKeyA;   // close the text (B on HGSS: declines phone calls)
    }
    else
    {
        e.stuck = 0;
    }
    e.lastX = r.x;
    e.lastZ = r.z;

    if (e.sinceProgress > kEggNoProgressMax)
        return eggStopWith(e, EggStop::NoEggs);

    if (e.phase != EggHunt::Hatching)
    {
        for (unsigned i = 0; i < 6; i++)
        {
            e.eggs[i] = i < r.count && r.eggs[i];
            e.sums[i] = i < r.count ? r.sums[i] : 0;
        }
        e.count = r.count;
    }
    return EggStep::None;
}

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
        {
            PC_LOG("battle ended");
            t.teamCheckFrame = t.frame + 30;   // fresh values right after the battle
        }
        t.inBattle = false;
        t.handled = false;
        teamTick(t, read, ctx);
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
    for (auto& s : t.lastTeam) s.clear();
    t.lastTeamCount = -1;
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

// ---- shiny auto-pause ---------------------------------------------------------

namespace
{
std::atomic<int> gAutoPause{-1};   // -1 = not read from the environment yet
}

bool autoPauseEnabled()
{
    int v = gAutoPause.load();
    if (v < 0)
    {
        const char* env = std::getenv("POKECOUNTER_AUTOPAUSE");
        v = (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F'
                     || ((env[0] == 'o' || env[0] == 'O') && (env[1] == 'f' || env[1] == 'F')))) ? 0 : 1;
        gAutoPause.store(v);
    }
    return v != 0;
}

void setAutoPauseEnabled(bool on)
{
    gAutoPause.store(on ? 1 : 0);
}

namespace
{
std::atomic<int> gTeam{-1};
}

bool teamEnabled()
{
    int v = gTeam.load();
    if (v < 0)
    {
        const char* env = std::getenv("POKECOUNTER_TEAM");
        v = (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F')) ? 0 : 1;
        gTeam.store(v);
    }
    return v != 0;
}

void setTeamEnabled(bool on)
{
    gTeam.store(on ? 1 : 0);
}

namespace
{
std::atomic<int> gBoxes{-1};
}

bool boxesEnabled()
{
    int v = gBoxes.load();
    if (v < 0)
    {
        const char* env = std::getenv("POKECOUNTER_BOXES");
        v = (env && (env[0] == '0' || env[0] == 'n' || env[0] == 'N' || env[0] == 'f' || env[0] == 'F')) ? 0 : 1;
        gBoxes.store(v);
    }
    return v != 0;
}

// The pause waits until the display has confirmed the report (or 1.5 s have
// passed): a paused emulator no longer sends, so the display would only learn
// about the shiny - and play its melody - after resuming.
constexpr uint64_t kShinyPauseWaitMs = 1500;

bool takeShinyEvent(Tracker& t)
{
    if (!t.shinyEvent.load())
        return false;
    if (!t.queue.empty() && nowMs() - t.shinyAtMs < kShinyPauseWaitMs)
        return false;
    return t.shinyEvent.exchange(false);
}
bool shinyHold(Tracker& t) { return t.shinyHold.load(); }
void releaseShinyHold(Tracker& t)
{
    t.shinyEvent.store(false);
    t.shinyHold.store(false);
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

bool takeShinyEvent(melonDS::NDS& nds) { return takeShinyEvent(trackerFor(&nds)); }
uint32_t encounterSeq(melonDS::NDS& nds) { return trackerFor(&nds).encounterSeq.load(); }
bool lastEncounterShiny(melonDS::NDS& nds) { return trackerFor(&nds).lastShiny.load(); }
bool gameSupported(melonDS::NDS& nds) { return trackerFor(&nds).supported; }

#ifndef POKECOUNTER_NO_MELONDS
const char* eggStart(melonDS::NDS& nds) { return eggStart(trackerFor(&nds), ramRead, &nds); }
void eggStop(melonDS::NDS& nds) { eggStop(trackerFor(&nds)); }
bool eggActive(melonDS::NDS& nds) { return eggActive(trackerFor(&nds)); }
uint32_t eggKeys(melonDS::NDS& nds) { return eggKeys(trackerFor(&nds)); }
EggStep eggAfterFrame(melonDS::NDS& nds) { return eggAfterFrame(trackerFor(&nds), ramRead, &nds); }
EggStop eggStopReason(melonDS::NDS& nds) { return eggStopReason(trackerFor(&nds)); }
uint32_t eggCycles(melonDS::NDS& nds) { return eggCycles(trackerFor(&nds)); }
uint32_t eggHatched(melonDS::NDS& nds) { return eggHatched(trackerFor(&nds)); }
#endif
bool shinyHold(melonDS::NDS& nds) { return shinyHold(trackerFor(&nds)); }
void releaseShinyHold(melonDS::NDS& nds) { releaseShinyHold(trackerFor(&nds)); }

bool takeAnyShinyEvent()
{
    std::lock_guard<std::mutex> lk(gTrackersLock);
    bool any = false;
    for (auto& kv : gTrackers)
        if (kv.second && takeShinyEvent(*kv.second))
            any = true;
    return any;
}

bool anyShinyHold()
{
    std::lock_guard<std::mutex> lk(gTrackersLock);
    for (auto& kv : gTrackers)
        if (kv.second && kv.second->shinyHold.load())
            return true;
    return false;
}

void releaseAllShinyHolds()
{
    std::lock_guard<std::mutex> lk(gTrackersLock);
    for (auto& kv : gTrackers)
        if (kv.second)
            releaseShinyHold(*kv.second);
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
    if (t.supported && boxesEnabled())
        processSave(t, nds.GetNDSSave(), nds.GetNDSSaveLength());
}
#endif

}
