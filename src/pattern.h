// Drum beats: styles with a verse and chorus groove, fills, an intro and an ending, all on General
// MIDI drum notes so every beat plays on every kit.
#pragma once
#include <string>
#include <vector>

namespace pattern {

struct Hit {
    int step;    // from the start of the part
    int note;    // General MIDI drum note
    int vel;     // 1..127
};

struct Part {
    int bars = 1;
    std::vector<Hit> hits;
    bool empty() const { return hits.empty(); }
};

// FILL_VERSE: the fill played in the verse, into the chorus; FILL_CHORUS: out of the chorus, into the verse.
// INTRO: the count-in. CLAP: the clap-along (the drums out, hand claps in).
enum Section { VERSE, CHORUS, FILL_VERSE, FILL_CHORUS, INTRO, ENDING, CLAP, SECTIONS };
const char *section_name(int s);
// the parts as an editor names them ("Fill to chorus", "Count-in", "Clap along"), and by their .beat
// section name ("fill", "fill-to-verse", "intro", "clap"...)
const char *part_label(int s);
const char *part_key(int s);
int part_by_key(const std::string &key);   // -1 when there's none


// claps on the backbeat, for the clap-along (2 and 4; 2 and 3 in 3/4; the big "2" in 6/8)
Part default_clap(const std::string &meter, int quarters, int steps);
bool same_part(const Part &a, const Part &b);

struct Style {
    std::string id, name, family;   // "bossa", "Bossa Nova", "Latin"
    std::string about;               // "inspired by..." (for the inspired beats)
    bool user = false;               // one you made (saved in your beats folder)
    int quarters = 4;                // quarter notes per bar (the MIDI clock counts these): 4/4 = 4, 3/4 and 6/8 = 3
    int steps = 16;                  // grid steps per bar: 16 = 16ths in 4/4, 12 = triplet eighths (shuffle, 12/8)
    std::string meter = "4/4";
    double swing = 0;                // 0 = straight .. 1 = full triplet swing of the off-beat steps
    int bpm = 100;                   // a good starting tempo
    Part parts[SECTIONS];
};

// every beat: the built-in ones, then yours (a snapshot; the beats themselves never move)
std::vector<const Style *> styles();
const Style *find(const std::string &id);

// your own beats, as .beat text files in dir
void load_user(const std::string &dir);
// save (a new beat, or a new version of one); returns the saved beat
const Style *save_user(const std::string &dir, const Style &s, std::string *err = nullptr);
bool delete_user(const std::string &dir, const std::string &id);
// a beat being built, to play without saving (id "edit-preview"; never listed)
const Style *preview(const Style &s);
std::string to_text(const Style &s);
// a beat from .beat text (false when it has no name or no verse)
bool from_text(const std::string &text, Style &s);
// a grid line for one drum in one part: "x...x...x...x..." (for editors)
std::string lane(const Style &s, int section, int note);
const std::vector<std::pair<std::string, int>> &drum_names();   // ("Kick", 36) ... for editors

// parse a grid like "K: x...x...x...x... | S: ....x.......x..." into hits (for custom beats)
Part parse(const std::string &grid, int steps_per_bar, std::string *err = nullptr);

}  // namespace pattern
