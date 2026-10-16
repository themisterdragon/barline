// Songs and set lists: each song is a text file (its beat, kit, tempo, loop length, the pedal slot
// to load, notes, and lyrics), and each set list is a file naming its songs in order.
//   ~/.local/share/barline/songs/thinking-out-loud.song
//   ~/.local/share/barline/setlists/Friday Gig.txt
#pragma once
#include <mutex>
#include <string>
#include <vector>

namespace songs {

struct Song {
    std::string id;            // the file name without .song ("thinking-out-loud")
    std::string name, artist, key, notes;
    std::string style = "pop";
    std::string kit;           // "" = keep the kit that's on
    double bpm = 100;
    int loop_bars = 4;
    int loop_bars_b = 0;     // track B (0 = the same as A)
    int slot = -1;             // the pedal's saved slot to load (-1 = none)
    int count_in = 1;
    double swing = -1;         // the feel for this song (-1 = the beat's own swing)
    double pocket = 0;         // ms: + laid back, - pushed
    double loose = -1;         // 0..1 (-1 = as set)
    int locked = -1;           // 1 locked to the grid, 0 human feel, -1 = as set
    std::string import_id;        // its id in the app it came from (so a re-import updates it)
    bool estimated = false;    // the tempo/beat/kit are a starting guess, not checked yet
    bool confirmed = false;    // set by you (an estimate never overwrites it)
    std::string lyrics;        // as typed: blank lines between verses
};

struct SetList {
    std::string name;
    std::vector<std::string> songs;   // song ids, in order
};

class Library {
public:
    explicit Library(const std::string &dir);   // the data folder
    void load();
    std::vector<Song> all();                      // A to Z
    bool get(const std::string &id, Song &out);
    // a new song (no id) gets one from its name; returns the id ("" on a problem)
    std::string save(Song s, std::string *err = nullptr);
    bool remove(const std::string &id);
    std::vector<SetList> setlists();
    bool get_setlist(const std::string &name, SetList &out);
    bool save_setlist(const SetList &l, std::string *err = nullptr);
    bool remove_setlist(const std::string &name);
    std::string dir() const { return dir_; }

private:
    std::string dir_;
    std::mutex mu_;
    std::vector<Song> songs_;
    std::vector<SetList> lists_;
};

std::string slug(const std::string &name);
// a set list name is a file name: keep it to letters, numbers, spaces, and a little punctuation
std::string clean_name(const std::string &name);

}  // namespace songs
