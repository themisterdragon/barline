// The networks this computer is on, for the iPad page: Wi-Fi, a cable, or a router plugged into a USB
// port (a travel router through a USB network adapter, or one that shares its network over USB).
// The page answers on all of them; this picks the one whose address Barline shows.
#pragma once
#include <cstdint>
#include <string>
#include <vector>

namespace net {

struct Net {
    std::string name;    // the system's name for it: "wlan0", "enx00e04c680001", "usb0"
    std::string kind;    // "usb" (plugged into a USB port), "wired", "wifi", "other"
    std::string ip;      // its IPv4 address ("" = none yet)
    uint32_t addr = 0, mask = 0;   // (host order) for "is that device on this network"
    bool up = false;     // a cable or radio link is there
    std::string label() const;   // "USB router (enx00e0…)", "Wi-Fi (wlan0)"
};

// every network but loopback, virtual ones (containers, VPNs), and the T2 Mac's own chip link
std::vector<Net> list();

// the network the page is shown on. want: "auto" (a USB router first, then a cable, then the
// network the computer goes out on), "usb", "wired", "wifi", or a network's name. Empty ip = none.
Net pick(const std::string &want);

// is this device (an IPv4 address) on that network? (this computer itself always is)
bool on(const Net &n, const std::string &ip);

}  // namespace net
