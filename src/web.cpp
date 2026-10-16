#include "web.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>
#include <mutex>
#include <random>
#include <sstream>

#include "http.h"
#include "net.h"
#include "paths.h"
#include "pattern.h"
#include "setlist_import.h"
#include "songs.h"
#include "version.h"

namespace web {

#include "web_page.inc"   // PAGE: the page itself (web/index.html, embedded by tools/embed_web.py)

namespace {

using clk = std::chrono::steady_clock;

// a private address: home and venue networks, a hotspot, this computer itself
bool private_ip(const std::string &ip) {
    unsigned a = 0, b = 0, c = 0, d = 0;
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u", &a, &b, &c, &d) != 4) return false;
    if (a == 127 || a == 10) return true;
    if (a == 192 && b == 168) return true;
    if (a == 172 && b >= 16 && b <= 31) return true;
    if (a == 169 && b == 254) return true;   // link-local (a cable straight between two devices)
    if (a == 100 && b >= 64 && b <= 127) return true;   // carrier-grade NAT (some phone hotspots)
    return false;
}

// the address the page was asked for has to be an IP or a .local name: a web page elsewhere can't
// point its own domain name at Barline and talk to it ("DNS rebinding")
bool good_host(const std::string &host) {
    std::string h = host.substr(0, host.rfind(':') == std::string::npos ? host.size() : host.rfind(':'));
    if (h.empty()) return false;
    bool ip = std::all_of(h.begin(), h.end(), [](char c) { return (c >= '0' && c <= '9') || c == '.'; });
    if (ip) return true;
    auto ends = [&](const std::string &suf) { return h.size() > suf.size() && h.compare(h.size() - suf.size(), suf.size(), suf) == 0; };
    return h == "localhost" || ends(".local");
}

std::string esc(const std::string &s) {
    std::string o;
    for (unsigned char c : s) {
        switch (c) {
        case '"': o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n"; break;
        case '\r': break;
        case '\t': o += "\\t"; break;
        default:
            if (c < 0x20) continue;
            o += (char)c;
        }
    }
    return o;
}

std::string form(const http::Request &r, const std::string &name) {
    http::Request copy;
    copy.query = r.body;
    return copy.param(name);
}

std::string random_hex(int bytes) {
    std::random_device rd;
    static const char *H = "0123456789abcdef";
    std::string o;
    for (int i = 0; i < bytes; i++) {
        unsigned v = rd() & 255;
        o += H[v >> 4];
        o += H[v & 15];
    }
    return o;
}

bool same(const std::string &a, const std::string &b) {   // compared in constant time
    if (a.size() != b.size()) return false;
    unsigned char d = 0;
    for (size_t i = 0; i < a.size(); i++) d |= (unsigned char)(a[i] ^ b[i]);
    return d == 0;
}

http::Response json(const std::string &body, int status = 200) {
    http::Response r;
    r.status = status;
    r.body = body;
    return r;
}

http::Response error(const std::string &msg, int status = 400) { return json("{\"error\":\"" + esc(msg) + "\"}", status); }

std::string song_json(const songs::Song &s) {
    std::ostringstream o;
    o << "{\"id\":\"" << esc(s.id) << "\",\"name\":\"" << esc(s.name) << "\",\"artist\":\"" << esc(s.artist) << "\",\"key\":\"" << esc(s.key)
      << "\",\"style\":\"" << esc(s.style) << "\",\"kit\":\"" << esc(s.kit) << "\",\"bpm\":" << s.bpm << ",\"loop_bars\":" << s.loop_bars << ",\"loop_bars_b\":" << s.loop_bars_b
      << ",\"slot\":" << s.slot << ",\"count_in\":" << s.count_in << ",\"swing\":" << s.swing << ",\"pocket\":" << s.pocket
      << ",\"loose\":" << s.loose << ",\"locked\":" << s.locked << ",\"estimated\":" << (s.estimated ? "true" : "false") << ",\"notes\":\"" << esc(s.notes) << "\",\"lyrics\":\"" << esc(s.lyrics) << "\"}";
    return o.str();
}

}  // namespace

struct Page::Impl {
    app::Control &ctl;
    http::Server server;
    std::mutex mu;
    std::map<std::string, std::vector<clk::time_point>> fails;   // wrong codes, by device
    explicit Impl(app::Control &c) : ctl(c) {}

    bool paired(const http::Request &r) {
        auto it = r.headers.find("x-barline");
        return it != r.headers.end() && it->second.size() == 32 && ctl.has_device(it->second);
    }

    // this computer's own window (the key Barline made for it this run), asking from this computer
    bool local(const http::Request &r, const std::string &token) {
        return !ctl.local_token.empty() && r.remote == "127.0.0.1" && same(token, ctl.local_token);
    }
    bool local(const http::Request &r) {
        auto it = r.headers.find("x-barline");
        return it != r.headers.end() && local(r, it->second);
    }

    http::Response handle(const http::Request &r) {
        if (!private_ip(r.remote)) return error("Only devices on this network.", 403);
        if (!ctl.lan && r.remote != "127.0.0.1") return error("The iPad page is turned off (web = no in the settings).", 403);
        if (ctl.cfg().network_only && !net::on(ctl.network(), r.remote)) return error("Barline only answers devices on " + ctl.network().label() + ".", 403);
        auto host = r.headers.find("host");
        if (host == r.headers.end() || !good_host(host->second)) return error("Open Barline by its address.", 403);
        if (r.method == "GET" && (r.path == "/" || r.path == "/index.html")) {
            http::Response p;
            p.type = "text/html; charset=utf-8";
            p.body = PAGE;
            return p;
        }
        if (r.method == "GET" && r.path == "/manifest.json") {
            http::Response p;
            p.type = "application/manifest+json";
            p.body = "{\"name\":\"Barline\",\"short_name\":\"Barline\",\"display\":\"standalone\",\"start_url\":\"/\",\"background_color\":\"#111\","
                     "\"theme_color\":\"#111\"}";
            return p;
        }
        if (r.method == "POST" && r.path == "/api/pair") return pair(r);
        // the window closing (or reloading): sent as it goes, so the key comes in the form, not a header
        if (r.method == "POST" && r.path == "/api/bye") {
            if (local(r, form(r, "token"))) ctl.window_bye();
            return json("{\"ok\":true}");
        }
        if (r.path.compare(0, 5, "/api/") == 0 && !paired(r)) return error("Not paired yet.", 401);
        if (r.method == "GET" && r.path == "/api/state") {
            if (local(r)) ctl.saw_window();
            return json(ctl.json());
        }
        if (r.method == "GET" && r.path == "/api/lists") return json(ctl.lists_json());
        if (r.method == "GET" && r.path == "/api/local") {   // only for this computer's window: how to pair the iPad
            if (!local(r)) return error("Only on this computer.", 403);
            return json("{\"address\":\"" + esc(ctl.address()) + "\",\"bookmark\":\"" + esc(ctl.bookmark()) + "\",\"code\":\"" +
                        esc(ctl.cfg().web_code) + "\",\"web\":" + (ctl.lan ? "true" : "false") + ",\"window_mode\":" +
                        (ctl.window_mode ? "true" : "false") + ",\"version\":\"" BARLINE_VERSION "\"}");
        }
        if (r.method == "POST" && r.path == "/api/act") {
            std::string a = form(r, "action");
            if (a == "quit" && !local(r)) return error("Quit from the computer.", 403);
            // a network change that would shut out the device asking for it: no (it couldn't undo it)
            if (a == "network" || a == "network_only") {
                std::string arg = form(r, "arg");
                bool only = a == "network_only" ? (arg == "on" || (arg != "off" && !ctl.cfg().network_only)) : ctl.cfg().network_only;
                net::Net n = a == "network" ? net::pick(arg) : ctl.network();
                if (only && !net::on(n, r.remote)) return error("That would shut this device out: it isn't on " + n.label() + ".", 409);
            }
            return ctl.act(a, form(r, "arg")) ? json("{\"ok\":true}") : error("Unknown action.");
        }
        if (r.method == "GET" && r.path == "/api/library") return library();
        if (r.method == "GET" && r.path == "/api/song") {
            songs::Song s;
            if (!ctl.library().get(r.param("id"), s)) return error("No such song.", 404);
            return json(song_json(s));
        }
        if (r.method == "POST" && r.path == "/api/song") return save_song(r);
        if (r.method == "POST" && r.path == "/api/song/delete") {
            ctl.library().remove(form(r, "id"));
            return json("{\"ok\":true}");
        }
        if (r.method == "GET" && r.path == "/api/beat") return beat(r.param("id"));
        if (r.method == "POST" && r.path == "/api/beat") return save_beat(r);
        if (r.method == "POST" && r.path == "/api/beat/delete") {
            const pattern::Style *b = pattern::find(form(r, "id"));
            if (!b || !b->user) return error("Only your own beats can be deleted.", 403);
            std::string name = b->name;
            pattern::delete_user(paths::data() + "/beats", form(r, "id"));
            ctl.note("Deleted the beat \"" + name + "\"");
            return json("{\"ok\":true}");
        }
        if (r.method == "POST" && r.path == "/api/setlist") return save_setlist(r);
        if (r.method == "POST" && r.path == "/api/setlist/delete") {
            ctl.library().remove_setlist(form(r, "name"));
            if (ctl.cfg().setlist == form(r, "name")) ctl.act("setlist", "");
            return json("{\"ok\":true}");
        }
        if (r.method == "POST" && r.path == "/api/import") return import(r);
        return error("Not found.", 404);
    }

    http::Response pair(const http::Request &r) {
        std::lock_guard<std::mutex> lock(mu);
        auto now = clk::now();
        auto &f = fails[r.remote];
        f.erase(std::remove_if(f.begin(), f.end(), [&](clk::time_point t) { return now - t > std::chrono::minutes(10); }), f.end());
        if (f.size() >= 10) return error("Too many wrong codes. Wait ten minutes.", 429);
        std::string code = form(r, "code");
        if (!same(code, ctl.cfg().web_code)) {
            f.push_back(now);
            return error("That's not the code. It's on the Barline screen.", 403);
        }
        std::string token = random_hex(16);
        ctl.add_device(token);
        ctl.note("Paired a device (" + r.remote + ")");
        return json("{\"token\":\"" + token + "\"}");
    }

    http::Response library() {
        std::ostringstream o;
        o << "{\"songs\":[";
        auto all = ctl.library().all();
        for (size_t i = 0; i < all.size(); i++)
            o << (i ? "," : "") << "{\"id\":\"" << esc(all[i].id) << "\",\"name\":\"" << esc(all[i].name) << "\",\"artist\":\"" << esc(all[i].artist)
              << "\",\"style\":\"" << esc(all[i].style) << "\",\"bpm\":" << all[i].bpm << ",\"lyrics\":" << (all[i].lyrics.empty() ? "false" : "true")
              << ",\"estimated\":" << (all[i].estimated ? "true" : "false") << "}";
        o << "],\"setlists\":[";
        auto lists = ctl.library().setlists();
        for (size_t i = 0; i < lists.size(); i++) {
            o << (i ? "," : "") << "{\"name\":\"" << esc(lists[i].name) << "\",\"songs\":[";
            for (size_t j = 0; j < lists[i].songs.size(); j++) o << (j ? "," : "") << "\"" << esc(lists[i].songs[j]) << "\"";
            o << "]}";
        }
        o << "],\"current\":\"" << esc(ctl.cfg().setlist) << "\"}";
        return json(o.str());
    }

    // a beat for the editor: its settings, and each part as [step, drum, loudness] hits
    http::Response beat(const std::string &id) {
        const pattern::Style *b = pattern::find(id);
        if (!b) return error("No such beat.", 404);
        std::ostringstream o;
        o << "{\"id\":\"" << esc(b->id) << "\",\"name\":\"" << esc(b->name) << "\",\"family\":\"" << esc(b->family) << "\",\"meter\":\"" << esc(b->meter)
          << "\",\"quarters\":" << b->quarters << ",\"steps\":" << b->steps << ",\"swing\":" << b->swing << ",\"bpm\":" << b->bpm
          << ",\"user\":" << (b->user ? "true" : "false") << ",\"parts\":{";
        // every part, by its .beat name: verse, chorus, fill (to the chorus), fill-to-verse, intro (the count-in), ending, clap
        for (int i = 0; i < pattern::SECTIONS; i++) {
            const pattern::Part &p = b->parts[i];
            o << (i ? "," : "") << "\"" << pattern::part_key(i) << "\":{\"bars\":" << std::max(1, p.bars) << ",\"hits\":[";
            for (size_t j = 0; j < p.hits.size(); j++) o << (j ? "," : "") << "[" << p.hits[j].step << "," << p.hits[j].note << "," << p.hits[j].vel << "]";
            o << "]}";
        }
        o << "},\"drums\":[";
        const auto &d = pattern::drum_names();
        for (size_t i = 0; i < d.size(); i++) o << (i ? "," : "") << "[\"" << esc(d[i].first) << "\"," << d[i].second << "]";
        o << "]}";
        return json(o.str());
    }

    // save a beat from the editor (its .beat text): yours is saved over, a built-in one (or "as new") as a new beat of yours;
    // with a song, that song gets the beat
    http::Response save_beat(const http::Request &r) {
        pattern::Style st;
        if (!pattern::from_text(form(r, "text"), st)) return error("A beat needs a name and something in the verse.");
        std::string id = form(r, "id");
        const pattern::Style *old = pattern::find(id);
        st.id = old && old->user && form(r, "as_new") != "1" ? id : "";
        std::string err;
        const pattern::Style *saved = pattern::save_user(paths::data() + "/beats", st, &err);
        if (!saved) return error("Couldn't save the beat: " + err);
        ctl.note("Saved the beat \"" + saved->name + "\"");
        std::string song = form(r, "song");
        songs::Song s;
        if (!song.empty() && ctl.library().get(song, s)) {
            s.style = saved->id;
            s.estimated = false;
            s.confirmed = true;
            ctl.library().save(s);
        }
        return json("{\"id\":\"" + esc(saved->id) + "\",\"name\":\"" + esc(saved->name) + "\"}");
    }

    http::Response save_song(const http::Request &r) {
        songs::Song s;
        std::string id = form(r, "id");
        if (!id.empty()) ctl.library().get(id, s);
        s.id = id;
        s.name = form(r, "name");
        s.artist = form(r, "artist");
        s.key = form(r, "key");
        s.style = form(r, "style");
        if (!pattern::find(s.style)) s.style = "pop";
        s.kit = form(r, "kit");
        s.bpm = std::max(30.0, std::min(300.0, std::atof(form(r, "bpm").c_str())));
        s.loop_bars = std::max(1, std::min(64, std::atoi(form(r, "loop_bars").c_str())));
        s.loop_bars_b = std::max(0, std::min(64, std::atoi(form(r, "loop_bars_b").c_str())));
        std::string slot = form(r, "slot");
        s.slot = slot.empty() ? -1 : std::max(-1, std::min(127, std::atoi(slot.c_str())));
        s.count_in = std::max(0, std::min(2, std::atoi(form(r, "count_in").c_str())));
        // the feel (left out: kept as it was)
        auto num = [&](const char *k, double lo, double hi, double &v) {
            std::string x = form(r, k);
            if (!x.empty()) v = std::max(lo, std::min(hi, std::atof(x.c_str())));
        };
        num("swing", -1, 1, s.swing);
        num("pocket", -15, 15, s.pocket);
        num("loose", -1, 1, s.loose);
        if (!form(r, "locked").empty()) s.locked = std::max(-1, std::min(1, std::atoi(form(r, "locked").c_str())));
        s.notes = form(r, "notes");
        s.lyrics = form(r, "lyrics");
        s.estimated = false;   // saved by you: checked
        s.confirmed = true;
        std::string err;
        std::string saved = ctl.library().save(s, &err);
        if (saved.empty()) return error(err);
        // a new song goes on the end of the set list in use
        if (id.empty() && !ctl.cfg().setlist.empty()) {
            songs::SetList l;
            if (ctl.library().get_setlist(ctl.cfg().setlist, l)) {
                l.songs.push_back(saved);
                ctl.library().save_setlist(l);
            }
        }
        if (ctl.current_song == saved) {
            songs::Song now;
            if (ctl.library().get(saved, now)) ctl.apply(now);
        }
        return json("{\"id\":\"" + esc(saved) + "\"}");
    }

    http::Response save_setlist(const http::Request &r) {
        songs::SetList l;
        l.name = songs::clean_name(form(r, "name"));
        std::string ids = form(r, "songs"), old = form(r, "old_name");
        std::istringstream in(ids);
        for (std::string id; std::getline(in, id, ',');)
            if (!id.empty()) l.songs.push_back(id);
        std::string err;
        if (!ctl.library().save_setlist(l, &err)) return error(err);
        if (!old.empty() && songs::clean_name(old) != l.name) {
            ctl.library().remove_setlist(old);
            if (ctl.cfg().setlist == old) ctl.act("setlist", l.name);
        }
        return json("{\"name\":\"" + esc(l.name) + "\"}");
    }

    http::Response import(const http::Request &r) {
        setlist_import::Result res = setlist_import::import(r.body, ctl.library());
        if (!res.error.empty()) return error(res.error);
        ctl.note("Imported " + std::to_string(res.songs) + " songs and " + std::to_string(res.setlists) + " set lists from a set list app");
        return json("{\"songs\":" + std::to_string(res.songs) + ",\"setlists\":" + std::to_string(res.setlists) + "}");
    }
};

Page::Page(app::Control &ctl) : impl_(new Impl(ctl)) {}
Page::~Page() { stop(); }

std::string Page::start(std::string *err) {
    std::string e;
    int port = impl_->server.start(impl_->ctl.cfg().web_port, [this](const http::Request &r) {
        http::Response res = impl_->handle(r);
        res.headers["Cache-Control"] = "no-store";
        res.headers["X-Frame-Options"] = "DENY";
        res.headers["X-Content-Type-Options"] = "nosniff";
        res.headers["Referrer-Policy"] = "no-referrer";
        res.headers["Content-Security-Policy"] = "default-src 'self'; script-src 'unsafe-inline'; style-src 'unsafe-inline'; img-src 'self' data:; media-src 'self' data: blob:";
        return res;
    }, e);
    if (!port) {
        if (err) *err = e;
        return "";
    }
    std::string ip = net::pick(impl_->ctl.cfg().network).ip;
    return "http://" + (ip.empty() ? std::string("this-computer") : ip) + ":" + std::to_string(port);
}

void Page::stop() { impl_->server.stop(); }
int Page::port() const { return impl_->server.port(); }

}  // namespace web
