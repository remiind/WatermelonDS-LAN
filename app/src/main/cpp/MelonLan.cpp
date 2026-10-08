// WatermelonDS-LAN: JNI bridge for melonDS' LAN multiplayer.
// See MelonLan.h for the threading model.

#include <jni.h>
#include <android/log.h>

#include <atomic>
#include <chrono>
#include <cstdio>
#include <mutex>
#include <string>
#include <vector>

#include "MelonLan.h"
#include "MPInterface.h"
#include "LAN.h"

using namespace melonDS;

namespace
{

constexpr const char* TAG = "MelonLan";

// If the emulator thread processed the MP interface within this window, the
// lobby poller leaves ENet alone.
constexpr int64_t kLoopActiveWindowMs = 250;

// Same default as desktop melonDS ("MP.RecvTimeout").
constexpr int kRecvTimeoutMs = 25;

std::recursive_mutex lanMutex;
std::atomic<int64_t> lastLoopProcessMs{0};

int64_t nowMs()
{
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

bool emulatorLoopActive()
{
    return nowMs() - lastLoopProcessMs.load(std::memory_order_acquire) < kLoopActiveWindowMs;
}

bool lanSelected()
{
    return MPInterface::GetType() == MPInterface_LAN;
}

LAN& lan()
{
    return static_cast<LAN&>(MPInterface::Get());
}

// LAN stores IPv4 addresses with the first octet in the most significant byte.
std::string ipToString(u32 ip)
{
    char buf[16];
    snprintf(buf, sizeof(buf), "%u.%u.%u.%u", ip >> 24, (ip >> 16) & 0xFF, (ip >> 8) & 0xFF, ip & 0xFF);
    return buf;
}

std::string fromJString(JNIEnv* env, jstring value)
{
    if (value == nullptr)
        return {};

    const char* chars = env->GetStringUTFChars(value, nullptr);
    if (chars == nullptr)
        return {};

    std::string result(chars);
    env->ReleaseStringUTFChars(value, chars);
    return result;
}

// Removes the field and record separators used by the Kotlin side, so a
// player or session name can never break the parsing.
std::string sanitize(const char* value, size_t maxLen)
{
    std::string result;
    for (size_t i = 0; i < maxLen && value[i] != '\0'; i++)
    {
        char c = value[i];
        result.push_back((c == '\t' || c == '\n') ? ' ' : c);
    }
    return result;
}

jobjectArray toJavaStringArray(JNIEnv* env, const std::vector<std::string>& values)
{
    jclass stringClass = env->FindClass("java/lang/String");
    jobjectArray array = env->NewObjectArray((jsize) values.size(), stringClass, nullptr);
    if (array == nullptr)
        return nullptr;

    for (size_t i = 0; i < values.size(); i++)
    {
        jstring entry = env->NewStringUTF(values[i].c_str());
        if (entry == nullptr)
            return nullptr;
        env->SetObjectArrayElement(array, (jsize) i, entry);
        env->DeleteLocalRef(entry);
    }

    return array;
}

}

namespace MelonLan
{

void loopProcess()
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    lastLoopProcessMs.store(nowMs(), std::memory_order_release);
    MPInterface::Get().Process();
}

}

extern "C"
{

// Switches the MP interface to LAN. Must not happen while a game is running,
// because the emulator thread holds on to the current interface.
JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeEnable(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (lanSelected())
        return JNI_TRUE;

    if (emulatorLoopActive())
    {
        __android_log_print(ANDROID_LOG_WARN, TAG, "refusing to switch MP interface while a game is running");
        return JNI_FALSE;
    }

    MPInterface::Set(MPInterface_LAN);
    MPInterface::Get().SetRecvTimeout(kRecvTimeoutMs);

    if (!lanSelected())
        return JNI_FALSE;

    __android_log_print(ANDROID_LOG_INFO, TAG, "MP interface switched to LAN");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeDisable(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (!lanSelected())
        return JNI_TRUE;

    if (emulatorLoopActive())
        return JNI_FALSE;

    // EndSession first: for a client, EndDiscovery() marks the session inactive,
    // after which EndSession() would skip disconnecting the peers.
    lan().EndSession();
    lan().EndDiscovery();
    MPInterface::Set(MPInterface_Dummy);
    __android_log_print(ANDROID_LOG_INFO, TAG, "MP interface switched back to dummy");
    return JNI_TRUE;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeIsEnabled(JNIEnv* env, jobject thiz)
{
    return lanSelected() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeIsGameRunning(JNIEnv* env, jobject thiz)
{
    return emulatorLoopActive() ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeStartHost(JNIEnv* env, jobject thiz, jstring playerName, jint maxPlayers)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (!lanSelected() || emulatorLoopActive())
        return JNI_FALSE;

    std::string name = fromJString(env, playerName);
    bool ok = lan().StartHost(name.c_str(), maxPlayers);
    __android_log_print(ANDROID_LOG_INFO, TAG, "StartHost(%d) -> %d", (int) maxPlayers, ok ? 1 : 0);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeStartDiscovery(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (!lanSelected())
        return JNI_FALSE;

    bool ok = lan().StartDiscovery();
    __android_log_print(ANDROID_LOG_INFO, TAG, "StartDiscovery -> %d", ok ? 1 : 0);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeEndDiscovery(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (lanSelected())
        lan().EndDiscovery();
}

// Blocks for up to ~5 seconds while ENet connects. Call from a worker thread.
JNIEXPORT jboolean JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeStartClient(JNIEnv* env, jobject thiz, jstring playerName, jstring host)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (!lanSelected() || emulatorLoopActive())
        return JNI_FALSE;

    std::string name = fromJString(env, playerName);
    std::string hostAddress = fromJString(env, host);

    lan().EndDiscovery();
    bool ok = lan().StartClient(name.c_str(), hostAddress.c_str());
    __android_log_print(ANDROID_LOG_INFO, TAG, "StartClient(%s) -> %d", hostAddress.c_str(), ok ? 1 : 0);
    return ok ? JNI_TRUE : JNI_FALSE;
}

JNIEXPORT void JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeEndSession(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (lanSelected())
        lan().EndSession();
}

// Lobby poller: drives discovery and connection handling while no game runs.
JNIEXPORT void JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativePoll(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    if (!lanSelected() || emulatorLoopActive())
        return;

    MPInterface::Get().Process();
}

// Each entry: "ip \t sessionName \t numPlayers \t maxPlayers \t status"
JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeGetDiscoveryList(JNIEnv* env, jobject thiz)
{
    std::vector<std::string> entries;
    {
        std::lock_guard<std::recursive_mutex> lock(lanMutex);
        if (lanSelected())
        {
            for (const auto& [address, data] : lan().GetDiscoveryList())
            {
                std::string entry = ipToString(address);
                entry += '\t';
                entry += sanitize(data.SessionName, sizeof(data.SessionName));
                entry += '\t';
                entry += std::to_string(data.NumPlayers);
                entry += '\t';
                entry += std::to_string(data.MaxPlayers);
                entry += '\t';
                entry += std::to_string(data.Status);
                entries.push_back(std::move(entry));
            }
        }
    }
    return toJavaStringArray(env, entries);
}

// Each entry: "id \t name \t status \t ip \t isLocal \t ping"
JNIEXPORT jobjectArray JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeGetPlayerList(JNIEnv* env, jobject thiz)
{
    std::vector<std::string> entries;
    {
        std::lock_guard<std::recursive_mutex> lock(lanMutex);
        if (lanSelected())
        {
            for (const auto& player : lan().GetPlayerList())
            {
                std::string entry = std::to_string(player.ID);
                entry += '\t';
                entry += sanitize(player.Name, sizeof(player.Name));
                entry += '\t';
                entry += std::to_string((int) player.Status);
                entry += '\t';
                entry += ipToString(player.Address);
                entry += '\t';
                entry += player.IsLocalPlayer ? "1" : "0";
                entry += '\t';
                entry += std::to_string(player.Ping);
                entries.push_back(std::move(entry));
            }
        }
    }
    return toJavaStringArray(env, entries);
}

JNIEXPORT jint JNICALL
Java_me_magnum_melonds_lan_MelonLan_nativeGetMaxPlayers(JNIEnv* env, jobject thiz)
{
    std::lock_guard<std::recursive_mutex> lock(lanMutex);
    return lanSelected() ? lan().GetMaxPlayers() : 0;
}

}
