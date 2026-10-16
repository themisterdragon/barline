#include "setlist_import.h"

#include <algorithm>
#include <cctype>
#include <map>
#include <set>
#include <vector>

#include "pattern.h"
#include "sqlite.h"

namespace setlist_import {

namespace {

std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string trim(std::string s) {
    while (!s.empty() && (s.back() == ' ' || s.back() == '\r' || s.back() == '\n' || s.back() == '\t')) s.pop_back();
    size_t i = 0;
    while (i < s.size() && (s[i] == ' ' || s[i] == '\t' || s[i] == '\r' || s[i] == '\n')) i++;
    return s.substr(i);
}

// a spreadsheet file (CSV, or tab-separated): rows of fields, quotes and line breaks inside quotes allowed
std::vector<std::vector<std::string>> table(const std::string &d) {
    char sep = ',';
    size_t nl = d.find('\n');
    std::string first = d.substr(0, nl);
    if (std::count(first.begin(), first.end(), '\t') > std::count(first.begin(), first.end(), ',')) sep = '\t';
    std::vector<std::vector<std::string>> rows(1);
    std::string cell;
    bool q = false;
    size_t i = 0;
    if (d.compare(0, 3, "\xEF\xBB\xBF") == 0) i = 3;   // a byte-order mark
    for (; i < d.size(); i++) {
        char c = d[i];
        if (q) {
            if (c == '"' && i + 1 < d.size() && d[i + 1] == '"') cell += '"', i++;
            else if (c == '"') q = false;
            else cell += c;
        } else if (c == '"') q = true;
        else if (c == sep) rows.back().push_back(cell), cell.clear();
        else if (c == '\n') {
            rows.back().push_back(cell);
            cell.clear();
            rows.emplace_back();
        } else if (c != '\r') cell += c;
    }
    rows.back().push_back(cell);
    while (!rows.empty() && rows.back().size() == 1 && trim(rows.back()[0]).empty()) rows.pop_back();
    return rows;
}

// a set list app's export keeps lyrics and notes as a little HTML: <p>...</p> paragraphs and <br /> lines
std::string html_text(const std::string &h) {
    if (h.find('<') == std::string::npos && h.find('&') == std::string::npos) return trim(h);
    std::string out;
    for (size_t i = 0; i < h.size();) {
        char c = h[i];
        if (c == '\r' || c == '\n') {   // in HTML a line break is only a space
            if (!out.empty() && out.back() != '\n' && out.back() != ' ') out += ' ';
            i++;
        } else if (c == '<') {
            size_t e = h.find('>', i);
            if (e == std::string::npos) break;
            std::string tag = lower(h.substr(i + 1, e - i - 1));
            std::string name = tag.substr(tag[0] == '/' ? 1 : 0);
            name = name.substr(0, name.find_first_of(" /"));
            while (!out.empty() && out.back() == ' ') out.pop_back();
            if (name == "br") out += '\n';
            else if (name == "p" || name == "div" || name == "h1" || name == "h2" || name == "h3" || name == "li") {
                if (tag[0] == '/' && !out.empty()) out += "\n\n";
            }
            i = e + 1;
        } else if (c == '&') {
            size_t e = h.find(';', i);
            std::string ent = e == std::string::npos || e - i > 10 ? "" : h.substr(i + 1, e - i - 1);
            static const std::map<std::string, std::string> E = {{"amp", "&"}, {"lt", "<"}, {"gt", ">"}, {"quot", "\""}, {"apos", "'"},
                                                                  {"nbsp", " "}, {"rsquo", "\xE2\x80\x99"}, {"lsquo", "\xE2\x80\x98"},
                                                                  {"rdquo", "\xE2\x80\x9D"}, {"ldquo", "\xE2\x80\x9C"}, {"hellip", "\xE2\x80\xA6"},
                                                                  {"mdash", "\xE2\x80\x94"}, {"ndash", "\xE2\x80\x93"}};
            if (!ent.empty() && E.count(ent)) {
                out += E.at(ent);
                i = e + 1;
            } else if (ent.size() > 1 && ent[0] == '#') {
                unsigned long cp = ent[1] == 'x' || ent[1] == 'X' ? std::strtoul(ent.c_str() + 2, nullptr, 16) : std::strtoul(ent.c_str() + 1, nullptr, 10);
                if (cp == 0 || cp > 0x10FFFF) cp = 0xFFFD;
                if (cp < 0x80) out += (char)cp;
                else if (cp < 0x800) out += (char)(0xC0 | (cp >> 6)), out += (char)(0x80 | (cp & 0x3F));
                else if (cp < 0x10000) out += (char)(0xE0 | (cp >> 12)), out += (char)(0x80 | ((cp >> 6) & 0x3F)), out += (char)(0x80 | (cp & 0x3F));
                else
                    out += (char)(0xF0 | (cp >> 18)), out += (char)(0x80 | ((cp >> 12) & 0x3F)), out += (char)(0x80 | ((cp >> 6) & 0x3F)),
                        out += (char)(0x80 | (cp & 0x3F));
                i = e + 1;
            } else {
                out += '&';
                i++;
            }
        } else {
            out += c;
            i++;
        }
    }
    // tidy: no spaces at line ends, at most one blank line in a row
    std::string tidy, line;
    int blanks = 0;
    out += '\n';
    for (char c : out) {
        if (c != '\n') {
            line += c;
            continue;
        }
        std::string t = trim(line);
        line.clear();
        if (t.empty()) {
            if (blanks++ == 0 && !tidy.empty()) tidy += '\n';
            continue;
        }
        blanks = 0;
        tidy += t + '\n';
    }
    return trim(tidy);
}

// a set list app's database export (a SQLite file): its songs, and its shows as set lists
Result import_db(const std::string &data, songs::Library &lib) {
    Result r;
    sqlite::Db db;
    std::string err;
    if (!db.open(data, &err)) {
        r.error = "Couldn't read that set list file (" + err + ").";
        return r;
    }
    auto existing = lib.all();
    std::map<std::string, std::string> ids;   // the app's song id -> Barline id
    bool ok = db.rows("song", [&](const std::map<std::string, sqlite::Value> &row) {
        auto f = [&](const char *k) {
            auto it = row.find(k);
            return it == row.end() ? std::string() : it->second.text();
        };
        std::string name = trim(f("name")), artist = trim(f("artist")), import_id_ = f("id");
        if (name.empty()) return;
        songs::Song s;
        bool found = false;
        for (const auto &e : existing)   // already here: update it (its beat and feel stay)
            if (!import_id_.empty() && e.import_id == import_id_) {
                s = e;
                found = true;
            }
        if (!found)
            for (const auto &e : existing)
                if (lower(e.name) == lower(name) && (lower(e.artist) == lower(artist) || e.artist.empty())) {
                    s = e;
                    found = true;
                }
        if (!found) {
            // a new song: a starting beat by its time signature (pick a better one later)
            std::string ts = f("timeSignature");
            s.style = ts == "3/4" ? "waltz" : ts == "6/8" ? "sixeight" : ts == "12/8" ? "twelveeight" : "songwriter";
        }
        s.name = name;
        s.artist = artist;
        s.import_id = import_id_;
        if (!trim(f("key")).empty()) s.key = trim(f("key"));
        double bpm = row.count("tempo") ? row.at("tempo").num() : 0;
        if (bpm >= 30 && bpm <= 300) s.bpm = bpm;
        std::string notes = html_text(f("notes"));
        for (auto &c : notes)
            if (c == '\n') c = ' ';
        if (!notes.empty()) s.notes = notes;
        std::string ly = html_text(f("lyrics"));
        if (ly.empty()) ly = html_text(f("chords"));
        if (!ly.empty()) s.lyrics = ly;
        std::string id = lib.save(s);
        if (!id.empty()) {
            ids[import_id_] = id;
            r.songs++;
        }
    }, &err);
    if (!ok) {
        r.error = "Couldn't read the songs in that file (" + err + ").";
        return r;
    }
    // shows become set lists, in their order ("-" marks a set break there; skipped)
    std::set<std::string> used;
    db.rows("show", [&](const std::map<std::string, sqlite::Value> &row) {
        std::string name = trim(row.count("name") ? row.at("name").text() : "");
        std::string date = row.count("date") ? row.at("date").text() : "";
        if (name.empty()) name = "Show " + date;
        if (used.count(songs::clean_name(name)) && !date.empty() && date != "0000-00-00") name += " " + date;
        for (int k = 2; used.count(songs::clean_name(name)); k++)   // still taken (no date to tell them apart): a number
            name = (row.count("name") ? trim(row.at("name").text()) : name) + " (" + std::to_string(k) + ")";
        songs::SetList l;
        l.name = name;
        std::string list = row.count("songs") ? row.at("songs").text() : "";
        size_t start = 0;
        while (start <= list.size()) {
            size_t comma = list.find(',', start);
            std::string id = list.substr(start, comma == std::string::npos ? std::string::npos : comma - start);
            if (ids.count(id)) l.songs.push_back(ids[id]);
            if (comma == std::string::npos) break;
            start = comma + 1;
        }
        if (l.songs.empty()) return;
        used.insert(songs::clean_name(l.name));
        if (lib.save_setlist(l)) r.setlists++;
    });
    return r;
}

}  // namespace

Result import(const std::string &data, songs::Library &lib) {
    Result r;
    if (data.empty()) {
        r.error = "The file is empty.";
        return r;
    }
    if (data.compare(0, 15, "SQLite format 3") == 0) return import_db(data, lib);   // a set list app's database export
    if (data.compare(0, 2, "PK") == 0) {
        r.error = "That's a zip file. Unzip it first, then import the .sqlite file inside.";
        return r;
    }
    auto rows = table(data);
    if (rows.size() < 2) {
        r.error = "That doesn't look like a song spreadsheet.";
        return r;
    }
    // which column is which (the set list app's names, and common ones)
    std::map<std::string, int> col;
    for (size_t c = 0; c < rows[0].size(); c++) {
        std::string h = lower(trim(rows[0][c]));
        auto is = [&](std::initializer_list<const char *> names) {
            for (const char *n : names)
                if (h == n) return true;
            return false;
        };
        if (is({"title", "name", "song", "song title", "song name"})) col["name"] = (int)c;
        else if (is({"artist", "band", "by", "composer"})) col["artist"] = (int)c;
        else if (is({"key", "starting key", "starting pitch"})) col["key"] = (int)c;
        else if (is({"tempo", "bpm", "beats per minute"})) col["bpm"] = (int)c;
        else if (is({"notes", "note", "comments"})) col["notes"] = (int)c;
        else if (is({"lyrics", "lyrics and chords", "words", "chords"})) col["lyrics"] = (int)c;
        else if (is({"style", "genre", "beat"})) col["style"] = (int)c;
        else if (is({"kit", "drums", "drum kit"})) col["kit"] = (int)c;
        else if (is({"swing"})) col["swing"] = (int)c;
        else if (is({"estimated", "estimate", "guess"})) col["estimated"] = (int)c;
    }
    if (!col.count("name")) {
        r.error = "The spreadsheet needs a Title (or Name) column.";
        return r;
    }
    auto get = [&](const std::vector<std::string> &row, const char *k) {
        auto it = col.find(k);
        return it == col.end() || it->second >= (int)row.size() ? std::string() : trim(row[(size_t)it->second]);
    };
    auto existing = lib.all();
    int skipped = 0;
    for (size_t i = 1; i < rows.size(); i++) {
        std::string name = get(rows[i], "name");
        if (name.empty()) continue;
        std::string artist = get(rows[i], "artist");
        bool estimate = !get(rows[i], "estimated").empty() && lower(get(rows[i], "estimated")) != "no";
        songs::Song s;
        bool found = false;
        for (const auto &e : existing)   // the same song (a song with no artist yet counts): update it, keeping its beat
            if (lower(e.name) == lower(name) && (lower(e.artist) == lower(artist) || e.artist.empty())) {
                s = e;
                found = true;
            }
        // an estimate only fills in songs you have, and never over what you've set yourself
        if (estimate && (!found || s.confirmed)) {
            skipped++;
            continue;
        }
        if (estimate) s.estimated = true;
        if (!get(rows[i], "style").empty()) {
            std::string st = get(rows[i], "style");
            if (pattern::find(st)) s.style = st;
            else
                for (const auto *p : pattern::styles())
                    if (lower(p->name) == lower(st)) s.style = p->id;
        }
        if (!get(rows[i], "kit").empty()) s.kit = get(rows[i], "kit");
        if (!get(rows[i], "swing").empty()) s.swing = std::max(-1.0, std::min(1.0, std::atof(get(rows[i], "swing").c_str())));
        s.name = name;
        s.artist = artist;
        if (!get(rows[i], "key").empty()) s.key = get(rows[i], "key");
        double bpm = std::atof(get(rows[i], "bpm").c_str());
        if (bpm >= 30 && bpm <= 300) s.bpm = bpm;
        if (!get(rows[i], "notes").empty()) s.notes = get(rows[i], "notes");
        if (!get(rows[i], "lyrics").empty()) s.lyrics = get(rows[i], "lyrics");
        if (!lib.save(s).empty()) r.songs++;
    }
    if (!r.songs) r.error = skipped ? "Nothing to fill in: those songs aren't here yet, or you've already set them." : "No songs found in that file.";
    return r;
}

}  // namespace setlist_import
