// A small HTTP/1.1 server for the home network: one request per connection, handled
// one at a time on a background thread. Plenty for a phone and a copy of Tally.
#pragma once
#include <atomic>
#include <functional>
#include <map>
#include <mutex>
#include <string>
#include <thread>

namespace http {

struct Request {
    std::string method, path, query, body;
    std::string remote;                            // the other device's IP address
    std::map<std::string, std::string> headers;   // names in lower case
    std::string param(const std::string &name) const;   // from the query string
};

struct Response {
    int status = 200;
    std::string type = "application/json; charset=utf-8";
    std::string body;
    std::map<std::string, std::string> headers;
};

using Handler = std::function<Response(const Request &)>;

class Server {
public:
    ~Server() { stop(); }
    // Tries `port`, then the next few if it's taken. Returns the port in use, or 0.
    int start(int port, Handler h, std::string &err);
    void stop();
    int port() const { return port_; }

private:
    void loop();
    std::thread th;
    std::atomic<bool> running{false};
    std::atomic<int> active{0};
    std::mutex per_ip_m;
    std::map<std::string, int> per_ip;   // open connections from each device
    long long sock = -1;
    int port_ = 0;
    Handler handler;
};

// The address other devices on the home network can use to reach this computer,
// like "192.168.1.20". Sends nothing (it only asks the system which network it would use).
std::string lan_address();
void open_in_browser(const std::string &url);
// opens `url` as an app window on this computer (a Chromium-family browser's --app window with its
// own profile in profile_dir, else Firefox, else the default browser); false if there's no browser
bool open_app_window(const std::string &url, const std::string &profile_dir = "");

}  // namespace http
