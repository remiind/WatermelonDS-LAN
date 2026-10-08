#ifndef MELONLAN_H
#define MELONLAN_H

// WatermelonDS-LAN: bridge between the Android frontend and melonDS' LAN
// multiplayer interface (src/net/LAN.cpp).
//
// Threading model:
//  * While a game is running, the emulator thread drives the MP interface by
//    calling MelonLan::loopProcess() once per frame (instead of calling
//    MPInterface::Get().Process() directly).
//  * While no game is running (lobby in the ROM list), the Kotlin side polls
//    nativePoll() from a background thread so discovery beacons are sent and
//    incoming connections are accepted.
//  * Both paths, as well as every lobby call (host/join/end), are serialized
//    through one mutex. The poller skips its work while the emulator thread has
//    processed recently, so ENet is never driven from two threads at once.

namespace MelonLan
{

// Called by the emulator thread once per emulated frame.
void loopProcess();

}

#endif // MELONLAN_H
