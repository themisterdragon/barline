#include "songs.h"
#include "sys.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>


namespace songs {

std::string slug(const std::string &name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
        else if (!out.empty() && out.back() != '-') out += '-';
        if (out.size() >= 60) break;
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "song" : out;
}

std::string clean_name(const std::string &name) {
    std::string out;
    for (char c : name)
        if (std::isalnum((unsigned char)c) || (unsigned char)c >= 0x80 || c == ' ' || c == '-' || c == '_' || c == '\'' || c == '&' || c == '!' || c == ',' ||
            c == '(' || c == ')' || c == '#' || c == '.')
            out += c;
    while (!out.empty() && out.back() == ' ') out.pop_back();
    while (!out.empty() && out[0] == ' ') out.erase(0, 1);
    if (out.size() > 60) out.resize(60);
    return out.empty() ? "Set List" : out;
}

Library::Library(const std::string &dir) : dir_(dir) {
    sys::make_dir(dir_ + "/songs", 0700);
    sys::make_dir(dir_ + "/setlists", 0700);
}

static std::vector<std::string> files_in(const std::string &dir, const std::string &ext) {
    std::vector<std::string> out;
    for (const auto &n : sys::list_dir(dir))
        if (n.size() > ext.size() && n.compare(n.size() - ext.size(), ext.size(), ext) == 0 && n[0] != '.') out.push_back(n);
    std::sort(out.begin(), out.end());
    return out;
}

static bool read_song(const std::string &path, Song &s) {
    std::ifstream f(path);
    if (!f) return false;
    std::string line;
    bool lyrics = false;
    std::ostringstream ly;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (lyrics) {
            ly << line << "\n";
            continue;
        }
        if (line == "[lyrics]") {
            lyrics = true;
            continue;
        }
        size_t eq = line.find('=');
        if (eq == std::string::npos || line[0] == '#') continue;
        std::string k = line.substr(0, eq), v = line.substr(eq + 1);
        while (!k.empty() && k.back() == ' ') k.pop_back();
        while (!v.empty() && v[0] == ' ') v.erase(0, 1);
        if (k == "name") s.name = v;
        else if (k == "artist") s.artist = v;
        else if (k == "key") s.key = v;
        else if (k == "notes") s.notes = v;
        else if (k == "style") s.style = v;
        else if (k == "kit") s.kit = v;
        else if (k == "bpm") s.bpm = std::max(30.0, std::min(300.0, std::atof(v.c_str())));
        else if (k == "loop_bars") s.loop_bars = std::max(1, std::min(64, std::atoi(v.c_str())));
        else if (k == "loop_bars_b") s.loop_bars_b = std::max(0, std::min(64, std::atoi(v.c_str())));
        else if (k == "slot") s.slot = std::max(-1, std::min(127, std::atoi(v.c_str())));
        else if (k == "count_in") s.count_in = std::max(0, std::min(2, std::atoi(v.c_str())));
        else if (k == "swing") s.swing = std::max(-1.0, std::min(1.0, std::atof(v.c_str())));
        else if (k == "pocket") s.pocket = std::max(-15.0, std::min(15.0, std::atof(v.c_str())));
        else if (k == "loose") s.loose = std::max(-1.0, std::min(1.0, std::atof(v.c_str())));
        else if (k == "import_id") s.import_id = v;
        else if (k == "estimated") s.estimated = v == "yes";
        else if (k == "confirmed") s.confirmed = v == "yes";
        else if (k == "locked") s.locked = std::max(-1, std::min(1, std::atoi(v.c_str())));
    }
    s.lyrics = ly.str();
    while (!s.lyrics.empty() && s.lyrics.back() == '\n') s.lyrics.pop_back();
    return !s.name.empty();
}

static bool write_file(const std::string &path, const std::string &text) { return sys::write_private(path, text); }

static std::string one_line(std::string s) {
    for (auto &c : s)
        if (c == '\n' || c == '\r') c = ' ';
    return s;
}

void Library::load() {
    std::lock_guard<std::mutex> lock(mu_);
    songs_.clear();
    for (const auto &n : files_in(dir_ + "/songs", ".song")) {
        Song s;
        s.id = n.substr(0, n.size() - 5);
        if (read_song(dir_ + "/songs/" + n, s)) songs_.push_back(s);
    }
    lists_.clear();
    for (const auto &n : files_in(dir_ + "/setlists", ".txt")) {
        SetList l;
        l.name = n.substr(0, n.size() - 4);
        std::ifstream f(dir_ + "/setlists/" + n);
        for (std::string line; std::getline(f, line);) {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (!line.empty() && line[0] != '#') l.songs.push_back(line);
        }
        lists_.push_back(l);
    }
}

std::vector<Song> Library::all() {
    std::lock_guard<std::mutex> lock(mu_);
    std::vector<Song> out = songs_;
    std::sort(out.begin(), out.end(), [](const Song &a, const Song &b) {
        std::string x = a.name, y = b.name;
        for (auto &c : x) c = (char)std::tolower((unsigned char)c);
        for (auto &c : y) c = (char)std::tolower((unsigned char)c);
        return x < y;
    });
    return out;
}

bool Library::get(const std::string &id, Song &out) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto &s : songs_)
        if (s.id == id) {
            out = s;
            return true;
        }
    return false;
}

std::string Library::save(Song s, std::string *err) {
    if (s.name.empty()) {
        if (err) *err = "A song needs a name.";
        return "";
    }
    std::lock_guard<std::mutex> lock(mu_);
    if (s.id.empty() || s.id.find('/') != std::string::npos || s.id[0] == '.') {
        std::string base = slug(s.name), id = base;
        auto taken = [&](const std::string &x) {
            for (const auto &o : songs_)
                if (o.id == x) return true;
            return false;
        };
        for (int k = 2; taken(id); k++) id = base + "-" + std::to_string(k);
        s.id = id;
    }
    std::ostringstream o;
    o << "name = " << one_line(s.name) << "\nartist = " << one_line(s.artist) << "\nkey = " << one_line(s.key) << "\nstyle = " << one_line(s.style)
      << "\nkit = " << one_line(s.kit) << "\nbpm = " << s.bpm << "\nloop_bars = " << s.loop_bars << "\nloop_bars_b = " << s.loop_bars_b << "\nslot = " << s.slot << "\ncount_in = " << s.count_in
      << "\nswing = " << s.swing << "\npocket = " << s.pocket << "\nloose = " << s.loose << "\nlocked = " << s.locked << "\nimport_id = " << one_line(s.import_id) << "\nestimated = " << (s.estimated ? "yes" : "no")
      << "\nconfirmed = " << (s.confirmed ? "yes" : "no") << "\nnotes = " << one_line(s.notes) << "\n[lyrics]\n" << s.lyrics << "\n";
    if (!write_file(dir_ + "/songs/" + s.id + ".song", o.str())) {
        if (err) *err = "Couldn't save the song.";
        return "";
    }
    bool found = false;
    for (auto &o2 : songs_)
        if (o2.id == s.id) {
            o2 = s;
            found = true;
        }
    if (!found) songs_.push_back(s);
    return s.id;
}

bool Library::remove(const std::string &id) {
    if (id.empty() || id.find('/') != std::string::npos || id[0] == '.') return false;
    std::lock_guard<std::mutex> lock(mu_);
    std::remove((dir_ + "/songs/" + id + ".song").c_str());
    songs_.erase(std::remove_if(songs_.begin(), songs_.end(), [&](const Song &s) { return s.id == id; }), songs_.end());
    return true;
}

std::vector<SetList> Library::setlists() {
    std::lock_guard<std::mutex> lock(mu_);
    return lists_;
}

bool Library::get_setlist(const std::string &name, SetList &out) {
    std::lock_guard<std::mutex> lock(mu_);
    for (const auto &l : lists_)
        if (l.name == name) {
            out = l;
            return true;
        }
    return false;
}

bool Library::save_setlist(const SetList &in, std::string *err) {
    SetList l = in;
    l.name = clean_name(l.name);
    std::ostringstream o;
    for (const auto &id : l.songs)
        if (!id.empty() && id.find('\n') == std::string::npos) o << id << "\n";
    std::lock_guard<std::mutex> lock(mu_);
    if (!write_file(dir_ + "/setlists/" + l.name + ".txt", o.str())) {
        if (err) *err = "Couldn't save the set list.";
        return false;
    }
    bool found = false;
    for (auto &x : lists_)
        if (x.name == l.name) {
            x = l;
            found = true;
        }
    if (!found) lists_.push_back(l);
    return true;
}

bool Library::remove_setlist(const std::string &name) {
    std::string n = clean_name(name);
    std::lock_guard<std::mutex> lock(mu_);
    std::remove((dir_ + "/setlists/" + n + ".txt").c_str());
    lists_.erase(std::remove_if(lists_.begin(), lists_.end(), [&](const SetList &l) { return l.name == n; }), lists_.end());
    return true;
}

}  // namespace songs
