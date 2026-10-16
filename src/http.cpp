#include "http.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cstring>
#include <vector>

#ifdef _WIN32
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <shellapi.h>
typedef SOCKET sock_t;
#define CLOSESOCK closesocket
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <unistd.h>
#include <fcntl.h>
#include <spawn.h>
#include <sys/wait.h>
#include <signal.h>
extern char **environ;
typedef int sock_t;
#define CLOSESOCK close
#define INVALID_SOCKET (-1)
#endif

namespace http {

namespace {

const size_t MAX_BODY = 32 * 1024 * 1024;   // room for a big imported song database

std::string reason(int s) {
    switch (s) {
        case 200: return "OK";
        case 204: return "No Content";
        case 400: return "Bad Request";
        case 401: return "Unauthorized";
        case 404: return "Not Found";
        case 405: return "Method Not Allowed";
        case 413: return "Payload Too Large";
        case 429: return "Too Many Requests";
        default: return "Error";
    }
}

int hexval(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string url_decode(const std::string &s) {
    std::string o;
    for (size_t i = 0; i < s.size(); i++) {
        if (s[i] == '%' && i + 2 < s.size() && hexval(s[i + 1]) >= 0 && hexval(s[i + 2]) >= 0) {
            o += (char)(hexval(s[i + 1]) * 16 + hexval(s[i + 2]));
            i += 2;
        } else o += s[i] == '+' ? ' ' : s[i];
    }
    return o;
}

bool send_all(sock_t s, const std::string &data) {
    size_t sent = 0;
    while (sent < data.size()) {
        int n = (int)send(s, data.data() + sent, (int)(data.size() - sent), 0);
        if (n <= 0) return false;
        sent += (size_t)n;
    }
    return true;
}

// Sockets shouldn't leak into a restarted copy of the program (after "Update from file").
void no_inherit(sock_t s) {
#ifdef _WIN32
    SetHandleInformation((HANDLE)s, HANDLE_FLAG_INHERIT, 0);
#else
    fcntl(s, F_SETFD, FD_CLOEXEC);
#endif
}

void serve_one(sock_t c, const std::string &remote, const Handler &handler) {
#ifdef _WIN32
    DWORD tv = 10000;
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, (const char *)&tv, sizeof tv);
#else
    timeval tv{10, 0};
    setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
    setsockopt(c, SOL_SOCKET, SO_SNDTIMEO, &tv, sizeof tv);
#endif
    static bool dbg = std::getenv("BARLINE_DEBUG") != nullptr;
    if (dbg) std::fprintf(stderr, "[conn %d] open\n", (int)c);
    std::string buf;
    char tmp[8192];
    size_t head_end = std::string::npos;
    // A whole request has to arrive in time, not just each piece: a device that trickles in a
    // byte at a time can't hold a connection open forever.
    auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(15);
    auto late = [&] { return std::chrono::steady_clock::now() > deadline; };
    while (head_end == std::string::npos) {
        if (late()) return;
        int n = (int)recv(c, tmp, sizeof tmp, 0);
        if (dbg) std::fprintf(stderr, "[conn %d] recv %d\n", (int)c, n);
        if (n <= 0) return;
        buf.append(tmp, (size_t)n);
        head_end = buf.find("\r\n\r\n");
        if (head_end == std::string::npos && buf.size() > 32768) return;
    }
    Request req;
    req.remote = remote;
    std::string head = buf.substr(0, head_end);
    size_t line_end = head.find("\r\n");
    std::string first = head.substr(0, line_end);
    size_t sp1 = first.find(' '), sp2 = first.rfind(' ');
    if (sp1 == std::string::npos || sp2 <= sp1) return;
    req.method = first.substr(0, sp1);
    std::string target = first.substr(sp1 + 1, sp2 - sp1 - 1);
    size_t q = target.find('?');
    req.path = url_decode(target.substr(0, q));
    if (q != std::string::npos) req.query = target.substr(q + 1);
    size_t pos = line_end == std::string::npos ? head.size() : line_end + 2;
    while (pos < head.size()) {
        size_t e = head.find("\r\n", pos);
        if (e == std::string::npos) e = head.size();
        std::string h = head.substr(pos, e - pos);
        size_t colon = h.find(':');
        if (colon != std::string::npos) {
            std::string k = h.substr(0, colon), v = h.substr(colon + 1);
            for (auto &ch : k) ch = (char)std::tolower((unsigned char)ch);
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            req.headers[k] = v;
        }
        pos = e + 2;
    }
    size_t want = (size_t)std::strtoul(req.headers["content-length"].c_str(), nullptr, 10);
    Response res;
    if (want > MAX_BODY) {
        res.status = 413;
        res.body = "{\"error\":\"Too big.\"}";
    } else {
        req.body = buf.substr(head_end + 4);
        deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);   // lyrics on slow Wi-Fi
        while (req.body.size() < want) {
            if (late()) return;
            int n = (int)recv(c, tmp, sizeof tmp, 0);
            if (n <= 0) return;
            req.body.append(tmp, (size_t)n);
        }
        req.body.resize(want);
        res = handler(req);
    }
    std::string out = "HTTP/1.1 " + std::to_string(res.status) + " " + reason(res.status) + "\r\n";
    out += "Content-Type: " + res.type + "\r\nContent-Length: " + std::to_string(res.body.size()) + "\r\n";
    out += "Connection: close\r\nX-Content-Type-Options: nosniff\r\nReferrer-Policy: no-referrer\r\n";
    if (!res.headers.count("Cache-Control")) out += "Cache-Control: no-store\r\n";
    for (const auto &h : res.headers) out += h.first + ": " + h.second + "\r\n";
    out += "\r\n";
    if (req.method != "HEAD") out += res.body;
    bool sent = send_all(c, out);
    if (dbg) std::fprintf(stderr, "[conn %d] %s %s -> %d (%zu bytes, sent=%d)\n", (int)c, req.method.c_str(), req.path.c_str(), res.status, out.size(), sent);
}

}  // namespace

std::string Request::param(const std::string &name) const {
    size_t start = 0;
    while (start <= query.size()) {
        size_t amp = query.find('&', start);
        if (amp == std::string::npos) amp = query.size();
        std::string kv = query.substr(start, amp - start);
        size_t eq = kv.find('=');
        if (url_decode(kv.substr(0, eq)) == name) return eq == std::string::npos ? "" : url_decode(kv.substr(eq + 1));
        start = amp + 1;
    }
    return "";
}

int Server::start(int port, Handler h, std::string &err) {
#ifdef _WIN32
    WSADATA w;
    WSAStartup(MAKEWORD(2, 2), &w);
#else
    signal(SIGPIPE, SIG_IGN);
#endif
    handler = std::move(h);
    for (int p = port; p < port + 10; p++) {
        sock_t s = socket(AF_INET, SOCK_STREAM, 0);
        if (s == INVALID_SOCKET) { err = "Couldn't open a network socket."; return 0; }
#ifndef _WIN32
        int one = 1;
        setsockopt(s, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
#endif
        sockaddr_in a{};
        a.sin_family = AF_INET;
        a.sin_addr.s_addr = htonl(INADDR_ANY);
        a.sin_port = htons((unsigned short)p);
        no_inherit(s);
        if (bind(s, (sockaddr *)&a, sizeof a) == 0 && listen(s, 16) == 0) {
            sock = (long long)s;
            port_ = p;
            running = true;
            th = std::thread([this] { loop(); });
            return p;
        }
        CLOSESOCK(s);
    }
    err = "Ports " + std::to_string(port) + "-" + std::to_string(port + 9) + " are all in use by other programs.";
    return 0;
}

void Server::loop() {
    while (running) {
        sockaddr_in from{};
        socklen_t from_len = sizeof from;
        sock_t c = accept((sock_t)sock, (sockaddr *)&from, &from_len);
        if (c == INVALID_SOCKET) {
            if (!running) break;
            continue;
        }
        char ip[64] = "?";
        inet_ntop(AF_INET, &from.sin_addr, ip, sizeof ip);
        std::string remote = ip;
        // Each connection gets its own short-lived thread: browsers open spare connections
        // and leave them idle, and one idle connection mustn't keep the phone waiting.
        no_inherit(c);
        // a phone opens a few at once; no one device gets to use up all the room
        {
            std::lock_guard<std::mutex> g(per_ip_m);
            if (active >= 32 || per_ip[remote] >= 8) { CLOSESOCK(c); continue; }
            per_ip[remote]++;
        }
        active++;
        std::thread([this, c, remote] {
            serve_one(c, remote, handler);
            CLOSESOCK(c);
            {
                std::lock_guard<std::mutex> g(per_ip_m);
                if (--per_ip[remote] <= 0) per_ip.erase(remote);
            }
            active--;
        }).detach();
    }
    // let replies that are being written finish (idle connections just get cut off at exit)
    for (int i = 0; i < 100 && active > 0; i++) std::this_thread::sleep_for(std::chrono::milliseconds(20));
}

void Server::stop() {
    if (!running) return;
    running = false;
#ifdef _WIN32
    closesocket((sock_t)sock);
#else
    shutdown((sock_t)sock, SHUT_RDWR);
    close((sock_t)sock);
#endif
    if (th.joinable()) th.join();
}

std::string lan_address() {
    // "Connecting" a UDP socket sends no packets; it just picks the outgoing network.
    sock_t s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return "127.0.0.1";
    sockaddr_in a{};
    a.sin_family = AF_INET;
    a.sin_port = htons(9);
    inet_pton(AF_INET, "192.0.2.1", &a.sin_addr);   // a documentation-only address, never reachable
    std::string out = "127.0.0.1";
    if (connect(s, (sockaddr *)&a, sizeof a) == 0) {
        sockaddr_in me{};
        socklen_t len = sizeof me;
        if (getsockname(s, (sockaddr *)&me, &len) == 0) {
            char b[64];
            if (inet_ntop(AF_INET, &me.sin_addr, b, sizeof b) && std::strcmp(b, "0.0.0.0") != 0) out = b;
        }
    }
    CLOSESOCK(s);
    return out;
}

void open_in_browser(const std::string &url) {
#ifdef _WIN32
    ShellExecuteA(nullptr, "open", url.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
#ifdef __APPLE__
    const char *prog = "open";
#else
    const char *prog = "xdg-open";
#endif
    pid_t pid;
    char *argv[] = {(char *)prog, (char *)url.c_str(), nullptr};
    if (posix_spawnp(&pid, prog, nullptr, nullptr, argv, environ) == 0) {
        std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    }
#endif
}

#ifndef _WIN32
static std::string in_path(const std::string &name) {
    const char *p = std::getenv("PATH");
    std::string path = p ? p : "/usr/local/bin:/usr/bin:/bin";
    size_t start = 0;
    while (start <= path.size()) {
        size_t end = path.find(':', start);
        if (end == std::string::npos) end = path.size();
        std::string full = (end > start ? path.substr(start, end - start) : std::string(".")) + "/" + name;
        if (access(full.c_str(), X_OK) == 0) return full;
        start = end + 1;
    }
    return "";
}

// runs a program on its own (its own session, its output thrown away so it can't mess up the screen)
static bool spawn_quiet(const std::vector<std::string> &args) {
    std::vector<char *> argv;
    for (const auto &a : args) argv.push_back((char *)a.c_str());
    argv.push_back(nullptr);
    posix_spawn_file_actions_t fa;
    posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 2, "/dev/null", O_WRONLY, 0);
    posix_spawnattr_t at;
    posix_spawnattr_init(&at);
#ifdef POSIX_SPAWN_SETSID
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETSID);
#endif
    pid_t pid;
    bool ok = posix_spawn(&pid, argv[0], &fa, &at, argv.data(), environ) == 0;
    posix_spawn_file_actions_destroy(&fa);
    posix_spawnattr_destroy(&at);
    if (ok) std::thread([pid] { int st; waitpid(pid, &st, 0); }).detach();
    return ok;
}
#endif

bool open_app_window(const std::string &url, const std::string &profile_dir) {
#ifdef _WIN32
    // Edge (every Windows 10/11 has it) or Chrome makes a plain app window, on a profile of its own
    std::vector<std::string> progs;
    for (const char *env : {"ProgramFiles(x86)", "ProgramFiles", "LOCALAPPDATA"}) {
        const char *base = std::getenv(env);
        if (!base) continue;
        progs.push_back(std::string(base) + "\\Microsoft\\Edge\\Application\\msedge.exe");
        progs.push_back(std::string(base) + "\\Google\\Chrome\\Application\\chrome.exe");
    }
    for (const auto &prog : progs) {
        if (GetFileAttributesA(prog.c_str()) == INVALID_FILE_ATTRIBUTES) continue;
        std::string cmd = "\"" + prog + "\" --app=" + url + " --no-first-run --no-default-browser-check --window-size=1100,820";
        if (!profile_dir.empty()) cmd += " \"--user-data-dir=" + profile_dir + "\"";
        STARTUPINFOA si{};
        si.cb = sizeof si;
        PROCESS_INFORMATION pi{};
        if (CreateProcessA(nullptr, &cmd[0], nullptr, nullptr, FALSE, DETACHED_PROCESS, nullptr, nullptr, &si, &pi)) {
            CloseHandle(pi.hThread);
            CloseHandle(pi.hProcess);
            return true;
        }
    }
    open_in_browser(url);
    return true;
#else
    // a Chromium-family browser makes a plain app window (no tabs or address bar), on a profile of its own
    std::vector<std::string> progs;
#ifdef __APPLE__
    for (const char *app : {"Google Chrome", "Microsoft Edge", "Brave Browser", "Chromium", "Vivaldi"})
        progs.push_back(std::string("/Applications/") + app + ".app/Contents/MacOS/" + app);
#endif
    for (const char *b : {"chromium", "chromium-browser", "google-chrome-stable", "google-chrome", "brave-browser", "brave", "microsoft-edge",
                          "vivaldi-stable", "vivaldi"})
        progs.push_back(in_path(b));
    for (const auto &prog : progs) {
        if (prog.empty() || access(prog.c_str(), X_OK) != 0) continue;
        std::vector<std::string> args = {prog, "--app=" + url, "--no-first-run", "--no-default-browser-check", "--window-size=1100,820"};
        if (!profile_dir.empty()) args.push_back("--user-data-dir=" + profile_dir);
        if (spawn_quiet(args)) return true;
    }
    std::string ff = in_path("firefox");
    if (!ff.empty() && spawn_quiet({ff, "--new-window", url})) return true;
#ifdef __APPLE__
    std::string op = "/usr/bin/open";
#else
    std::string op = in_path("xdg-open");
#endif
    return !op.empty() && spawn_quiet({op, url});
#endif
}

}  // namespace http
