#include "net.h"

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <iphlpapi.h>
#else
#include <arpa/inet.h>
#include <ifaddrs.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <cctype>
#include <climits>
#include <cstdlib>
#include <fstream>

#include "http.h"

namespace net {

#ifdef __linux__
namespace {

bool exists(const std::string &p) {
    struct stat st;
    return stat(p.c_str(), &st) == 0;
}

std::string first_line(const std::string &p) {
    std::ifstream f(p);
    std::string s;
    std::getline(f, s);
    return s;
}

// where the network's hardware sits ("/sys/devices/pci0000:00/.../usb3/3-1/3-1:1.0"), or "" (virtual)
std::string device_path(const std::string &name) {
    char buf[PATH_MAX];
    std::string link = "/sys/class/net/" + name + "/device";
    if (!realpath(link.c_str(), buf)) return "";
    return buf;
}

std::string kind_of(const std::string &name, const std::string &dev) {
    if (exists("/sys/class/net/" + name + "/wireless") || exists("/sys/class/net/" + name + "/phy80211")) return "wifi";
    if (dev.empty()) return "";   // virtual: a bridge, a container's, a VPN's
    if (dev.find("/usb") != std::string::npos) return "usb";
    return "wired";
}

// a T2 Mac's link to its own security chip shows up as a USB network ("Apple T2 Controller"); it
// never leads anywhere
bool t2_chip(const std::string &dev) {
    if (dev.empty()) return false;
    std::string up = dev.substr(0, dev.rfind('/'));   // the USB device the interface belongs to
    return first_line(up + "/idVendor") == "05ac" && first_line(up + "/product").find("T2") != std::string::npos;
}

}  // namespace
#endif

std::string Net::label() const {
    if (name.empty()) return kind == "usb" ? "USB router (not plugged in)" : "No network";
    std::string what = kind == "usb" ? "USB router" : kind == "wifi" ? "Wi-Fi" : kind == "wired" ? "Cable" : "Network";
    return what + " (" + name + ")";
}

#ifdef __linux__
std::vector<Net> list() {
    std::vector<Net> out;
    // every network the system has, with or without an address yet
    {
        std::ifstream f("/proc/net/dev");
        std::string line;
        while (std::getline(f, line)) {
            size_t c = line.find(':');
            if (c == std::string::npos) continue;
            std::string name = line.substr(0, c);
            name.erase(0, name.find_first_not_of(' '));
            if (name.empty() || name == "lo") continue;
            std::string dev = device_path(name);
            Net n;
            n.name = name;
            n.kind = kind_of(name, dev);
            if (n.kind.empty() || t2_chip(dev)) continue;
            n.up = first_line("/sys/class/net/" + name + "/carrier") == "1";
            out.push_back(n);
        }
    }
    ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) == 0) {
        for (ifaddrs *i = ifs; i; i = i->ifa_next) {
            if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_name) continue;
            for (auto &n : out) {
                if (n.name != i->ifa_name || !n.ip.empty()) continue;
                auto *a = (sockaddr_in *)i->ifa_addr;
                char b[INET_ADDRSTRLEN];
                if (!inet_ntop(AF_INET, &a->sin_addr, b, sizeof b)) continue;
                n.ip = b;
                n.addr = ntohl(a->sin_addr.s_addr);
                n.mask = i->ifa_netmask ? ntohl(((sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr) : 0xffffff00u;
            }
        }
        freeifaddrs(ifs);
    }
    return out;
}

#elif defined(_WIN32)
// Windows says what each network is: Wi-Fi by its type, a USB adapter (or a router sharing over USB)
// by its description
std::vector<Net> list() {
    std::vector<Net> out;
    ULONG len = 16 * 1024;
    std::vector<unsigned char> buf(len);
    ULONG r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                   (IP_ADAPTER_ADDRESSES *)buf.data(), &len);
    if (r == ERROR_BUFFER_OVERFLOW) {
        buf.resize(len);
        r = GetAdaptersAddresses(AF_INET, GAA_FLAG_SKIP_ANYCAST | GAA_FLAG_SKIP_MULTICAST | GAA_FLAG_SKIP_DNS_SERVER, nullptr,
                                 (IP_ADAPTER_ADDRESSES *)buf.data(), &len);
    }
    if (r != NO_ERROR) return out;
    for (auto *a = (IP_ADAPTER_ADDRESSES *)buf.data(); a; a = a->Next) {
        if (a->IfType == IF_TYPE_SOFTWARE_LOOPBACK || a->IfType == IF_TYPE_TUNNEL) continue;
        char desc[256] = {};
        WideCharToMultiByte(CP_UTF8, 0, a->Description, -1, desc, sizeof desc - 1, nullptr, nullptr);
        std::string d = desc;
        std::string low = d;
        for (auto &c : low) c = (char)std::tolower((unsigned char)c);
        if (low.find("virtual") != std::string::npos || low.find("hyper-v") != std::string::npos || low.find("vpn") != std::string::npos) continue;
        Net n;
        char fn[128] = {};
        WideCharToMultiByte(CP_UTF8, 0, a->FriendlyName, -1, fn, sizeof fn - 1, nullptr, nullptr);
        n.name = fn;
        n.kind = a->IfType == IF_TYPE_IEEE80211 ? "wifi"
               : low.find("usb") != std::string::npos || low.find("rndis") != std::string::npos || low.find("remote ndis") != std::string::npos ? "usb"
                                                                                                                                              : "wired";
        n.up = a->OperStatus == IfOperStatusUp;
        for (auto *u = a->FirstUnicastAddress; u; u = u->Next) {
            if (u->Address.lpSockaddr->sa_family != AF_INET) continue;
            auto *sin = (sockaddr_in *)u->Address.lpSockaddr;
            char b[INET_ADDRSTRLEN];
            if (!inet_ntop(AF_INET, &sin->sin_addr, b, sizeof b)) continue;
            if (std::string(b).rfind("169.254.", 0) == 0) continue;   // no address handed out yet
            n.ip = b;
            n.addr = ntohl(sin->sin_addr.s_addr);
            unsigned bits = u->OnLinkPrefixLength;
            n.mask = bits >= 32 ? 0xffffffffu : bits == 0 ? 0 : ~((1u << (32 - bits)) - 1);
            break;
        }
        out.push_back(n);
    }
    return out;
}
#else
// macOS: every network with an address (Wi-Fi, a cable, and a USB adapter all look alike from here:
// pick one by name, or Auto takes the one the computer goes out on)
std::vector<Net> list() {
    std::vector<Net> out;
    ifaddrs *ifs = nullptr;
    if (getifaddrs(&ifs) != 0) return out;
    for (ifaddrs *i = ifs; i; i = i->ifa_next) {
        if (!i->ifa_addr || i->ifa_addr->sa_family != AF_INET || !i->ifa_name || (i->ifa_flags & IFF_LOOPBACK)) continue;
        std::string name = i->ifa_name;
        if (name.rfind("en", 0) != 0 && name.rfind("bridge", 0) != 0) continue;   // not utun (VPNs), awdl, and the like
        auto *a = (sockaddr_in *)i->ifa_addr;
        char b[INET_ADDRSTRLEN];
        if (!inet_ntop(AF_INET, &a->sin_addr, b, sizeof b)) continue;
        Net n;
        n.name = name;
        n.kind = "other";
        n.ip = b;
        n.addr = ntohl(a->sin_addr.s_addr);
        n.mask = i->ifa_netmask ? ntohl(((sockaddr_in *)i->ifa_netmask)->sin_addr.s_addr) : 0xffffff00u;
        n.up = (i->ifa_flags & IFF_RUNNING) != 0;
        out.push_back(n);
    }
    freeifaddrs(ifs);
    return out;
}
#endif

Net pick(const std::string &want) {
    std::vector<Net> all = list();
    auto with_ip = [&](const std::string &kind) -> const Net * {
        for (const auto &n : all)
            if (n.kind == kind && !n.ip.empty()) return &n;
        return nullptr;
    };
    if (want != "auto" && want != "usb" && want != "wired" && want != "wifi" && !want.empty()) {
        for (const auto &n : all)
            if (n.name == want) return n;
        Net none;
        none.name = want;
        none.kind = "other";
        return none;   // not plugged in
    }
    if (want == "usb" || want == "wired" || want == "wifi") {
        if (const Net *n = with_ip(want)) return *n;
        for (const auto &n : all)
            if (n.kind == want) return n;   // there, but no address yet
        Net none;
        none.kind = want;
        return none;
    }
    // auto: a router plugged in by USB (it was plugged in for this), then a cable, then the network
    // the computer goes out on
    if (const Net *n = with_ip("usb")) return *n;
    if (const Net *n = with_ip("wired")) return *n;
    std::string out = http::lan_address();
    for (const auto &n : all)
        if (n.ip == out) return n;
    for (const auto &n : all)
        if (!n.ip.empty()) return n;
    return Net();
}

bool on(const Net &n, const std::string &ip) {
    if (ip.rfind("127.", 0) == 0) return true;
    in_addr a{};
    if (n.ip.empty() || inet_pton(AF_INET, ip.c_str(), &a) != 1) return false;
    return (ntohl(a.s_addr) & n.mask) == (n.addr & n.mask);
}

}  // namespace net
