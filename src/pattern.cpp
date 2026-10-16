#include "pattern.h"
#include "sys.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <deque>
#include <fstream>
#include <mutex>
#include <map>
#include <sstream>

namespace pattern {

const char *section_name(int s) {
    static const char *N[] = {"Verse", "Chorus", "Fill", "Fill", "Intro", "Ending", "Clap along"};
    return s >= 0 && s < SECTIONS ? N[s] : "";
}

// Short names for the drums in the grids below (General MIDI notes)
static const std::map<std::string, int> DRUM = {
    {"K", 36},  {"K2", 35}, {"S", 38},  {"R", 37},  {"C", 40},  {"CL", 39}, {"H", 42},  {"P", 44},  {"O", 46},
    {"F", 43},  {"F2", 41}, {"T3", 45}, {"T2", 47}, {"T1", 48}, {"Y", 49},  {"Y2", 57}, {"D", 51},  {"B", 53},
    {"CH", 52}, {"SP", 55}, {"TB", 54}, {"CB", 56}, {"SH", 70}, {"CA", 69}, {"CV", 75}, {"WB", 76}, {"BH", 60},
    {"BL", 61}, {"QM", 62}, {"QH", 63}, {"QL", 64}, {"GU", 73}, {"TR", 81}};

// X accent, x normal, o soft, g ghost, . rest (spaces and | are only for reading)
static int vel_of(char c) {
    switch (c) {
    case 'X': return 124;
    case 'x': return 100;
    case 'o': return 72;
    case 'g': return 42;
    }
    return 0;
}

Part parse(const std::string &grid, int steps, std::string *err) {
    Part p;
    std::istringstream in(grid);
    int longest = 0;
    for (std::string line; std::getline(in, line);) {
        size_t colon = line.find(':');
        if (colon == std::string::npos) continue;
        std::string name;
        for (char c : line.substr(0, colon))
            if (!std::isspace((unsigned char)c)) name += (char)std::toupper((unsigned char)c);
        auto d = DRUM.find(name);
        int note = d != DRUM.end() ? d->second : std::atoi(name.c_str());
        if (note <= 0 || note > 127) {
            if (err) *err = "unknown drum " + name;
            continue;
        }
        int step = 0;
        for (char c : line.substr(colon + 1)) {
            if (std::isspace((unsigned char)c) || c == '|') continue;
            int v = vel_of(c);
            if (v) p.hits.push_back({step, note, v});
            step++;
        }
        longest = std::max(longest, step);
    }
    p.bars = std::max(1, (longest + steps - 1) / steps);
    return p;
}

namespace {

struct Def {
    const char *id, *name, *family, *meter;
    int quarters, steps;
    double swing;
    int bpm;
    const char *verse, *chorus, *fill, *intro, *ending;
    const char *about;
};

// The fills when a style has none of its own: toms down the last two beats
const char *FILL16 = "K: x...............\nH: x.x.x.x.........\nS: ....x...xx......\nT1: ..........xx....\nT3: ............xx..\nF: ..............xx";
const char *FILL12 = "K: x...........\nD: x..x..........\nS: ......xxx...\nT1: .........xx.\nF: ...........x";
const char *FILL34 = "K: x...........\nH: x.x.x.......\nS: ......xx....\nT1: ........xx..\nF: ..........xx";

const Def DEFS[] = {
    // ---- rock
    {"rock", "Rock", "Rock", "4/4", 4, 16, 0, 112,
     "K: x.......x.x.....\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.......x.x.....\nS: ....x.......x...\nO: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"rock16", "Rock 16ths", "Rock", "4/4", 4, 16, 0, 96,
     "K: x.....x.x.......\nS: ....x.......x...\nH: XoxoXoxoXoxoXoxo",
     "K: x.....x.x..x....\nS: ....x.......x...\nD: x.x.x.x.x.x.x.x.\nB: ....x.......x...",
     nullptr, nullptr, nullptr, nullptr},
    {"halftime", "Half-Time", "Rock", "4/4", 4, 16, 0, 80,
     "K: x.........x.....\nS: ........x.......\nH: x.x.x.x.x.x.x.x.",
     "K: x.....x...x.....\nS: ........x.......\nD: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"punk", "Punk", "Rock", "4/4", 4, 16, 0, 176,
     "K: x...x...x...x...\nS: ..x...x...x...x.\nH: x.x.x.x.x.x.x.x.",
     "K: x.x.x.x.x.x.x.x.\nS: ....x.......x...\nO: x...x...x...x...",
     "K: x...............\nS: xxxxxxxxxxxxxxxx", nullptr, nullptr, nullptr},
    // ---- pop
    {"pop", "Pop", "Pop", "4/4", 4, 16, 0, 104,
     "K: x.....x...x.....\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.....x.x.x.....\nS: ....x.......x...\nCL: ....x.......x...\nO: ..x...x...x...x.\nH: x...x...x...x...",
     nullptr, nullptr, nullptr, nullptr},
    {"ballad", "Pop Ballad", "Pop", "4/4", 4, 16, 0, 72,
     "K: x.......x.....x.\nR: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.......x.....x.\nS: ....x.......x...\nD: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"songwriter", "Singer-Songwriter", "Acoustic", "4/4", 4, 16, 0, 92,
     "K: x.......x.......\nR: ....x.......x...\nSH: x.o.x.o.x.o.x.o.",
     "K: x.....x.x.......\nS: ....x.......x...\nTB: ....x.......x...\nSH: xoxoxoxoxoxoxoxo",
     nullptr, nullptr, nullptr, nullptr},
    {"floortom", "Floor Tom Groove", "Acoustic", "4/4", 4, 16, 0, 104,
     "K: x.......x.......\nF: x..x..x.x..x..x.\nS: ....x.......x...",
     "K: x.....x.x.......\nF: x..x..x.x..x..x.\nS: ....x.......x...\nO: ..x...x...x...x.",
     nullptr, nullptr, nullptr, nullptr},
    {"cajon", "Cajon Groove", "Acoustic", "4/4", 4, 16, 0, 96,
     "K: x.....x...x.....\nS: ....x.......x..o\nR: ...g.....g......\nSH: xoxoxoxoxoxoxoxo",
     "K: x.....x.x.x.....\nS: ....x.......x.x.\nR: ...g.g...g......\nTB: ....x.......x...\nSH: xoxoxoxoxoxoxoxo",
     "K: x...............\nS: ....x...x.x.xxxx\nSH: xoxoxoxo........", nullptr, nullptr, nullptr},
    {"folk", "Folk Stomp", "Acoustic", "4/4", 4, 16, 0, 120,
     "K: x...x...x...x...\nTB: ..x...x...x...x.\nCL: ....x.......x...",
     "K: x...x...x...x...\nS: ....x.......x...\nTB: x.x.x.x.x.x.x.x.\nCL: ....x.......x...",
     "K: x...x...x...x...\nCL: ....x...x.x.x.x.", nullptr, nullptr, nullptr},
    // ---- dance
    {"four", "Four on the Floor", "Dance", "4/4", 4, 16, 0, 122,
     "K: x...x...x...x...\nCL: ....x.......x...\nO: ..x...x...x...x.\nH: o.o.o.o.o.o.o.o.",
     "K: x...x...x...x...\nS: ....x.......x...\nCL: ....x.......x...\nO: ..x...x...x...x.\nH: oooooooooooooooo\nTB: ..x...x...x...x.",
     "K: x...x...x...x...\nS: ....x...xxxxxxxx", nullptr, nullptr, nullptr},
    {"disco", "Disco", "Dance", "4/4", 4, 16, 0, 118,
     "K: x...x...x...x...\nS: ....x.......x...\nH: xoxoxoxoxoxoxoxo\nO: ..x...x...x...x.",
     "K: x...x...x...x...\nS: ....x.......x...\nO: ..x...x...x...x.\nH: x.x.x.x.x.x.x.x.\nCB: x...x...x...x...",
     nullptr, nullptr, nullptr, nullptr},
    // ---- soul, funk, R&B, hip-hop
    {"motown", "Motown", "Soul", "4/4", 4, 16, 0, 124,
     "K: x.....x.x.......\nS: o...X...o...X...\nTB: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.....x.x.x.....\nS: x...X...x...X...\nTB: x.x.x.x.x.x.x.x.\nH: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"funk", "Funk", "Soul", "4/4", 4, 16, 0, 100,
     "K: x.x.......x..x..\nS: ....x..g.g..x..g\nH: xoxoxoxoxoxoxox.\nO: ..............x.",
     "K: x.x...x...x..x..\nS: ....x..g.g..x..g\nH: xoxoxoxoxoxoxoxo\nCB: x...x...x...x...",
     "K: x.x.............\nS: ....x..g.gxgxxxx\nH: xoxoxoxo........", nullptr, nullptr, nullptr},
    {"boombap", "Boom Bap", "Hip-Hop", "4/4", 4, 16, 0.55, 90,
     "K: x......x.xx.....\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x......x.xx...x.\nS: ....x.......x...\nCL: ....x.......x...\nH: xxxxxxxxxxxxxxxx",
     "K: x......x........\nS: ....x.......x.xx\nH: x.x.x.x.x.x.....", nullptr, nullptr, nullptr},
    {"rnb", "R&B Slow Jam", "Soul", "4/4", 4, 16, 0.4, 68,
     "K: x......x..x.....\nCL: ....x.......x...\nH: xoxoxoxoxoxoxoxo",
     "K: x......x..x..x..\nS: ....x.......x...\nCL: ....x.......x...\nH: xoxoxoxoxoxoxoxo\nO: ..............x.",
     nullptr, nullptr, nullptr, nullptr},
    // ---- country
    {"train", "Train Beat", "Country", "4/4", 4, 16, 0, 124,
     "K: x.......x.......\nS: ggggXgggggggXggg",
     "K: x.......x.......\nS: ggggXgggggggXggg\nH: ..x...x...x...x.",
     nullptr, nullptr, nullptr, nullptr},
    {"twostep", "Country Two-Step", "Country", "4/4", 4, 16, 0, 112,
     "K: x.......x.......\nR: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.......x.......\nS: ....x.......x...\nD: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    // ---- swing and shuffle (a triplet grid: 3 steps a beat)
    {"shuffle", "Blues Shuffle", "Blues", "4/4 shuffle", 4, 12, 0, 112,
     "K: x.....x.....\nS: ...x.....x..\nH: x.xx.xx.xx.x",
     "K: x.....x.....\nS: ...x.....x..\nD: x.xx.xx.xx.x",
     nullptr, nullptr, nullptr, nullptr},
    {"halfshuffle", "Half-Time Shuffle", "Blues", "4/4 shuffle", 4, 12, 0, 84,
     "K: x.......x...\nS: ......X...g.\nH: x.xx.xx.xx.x\nR: .g...g...g..",
     "K: x.......x...\nS: ......X...g.\nO: x.xx.xx.xx.x",
     nullptr, nullptr, nullptr, nullptr},
    {"swing", "Jazz Swing", "Jazz", "4/4 swing", 4, 12, 0, 140,
     "D: x..x.xx..x.x\nP: ...x.....x..\nK: g..g..g..g..",
     "D: x..x.xx..x.x\nP: ...x.....x..\nK: g..g..g..g..\nS: ....g.....g.",
     "D: x..x........\nS: ......x.xx.x\nK: ...........x", nullptr, nullptr, nullptr},
    {"jazzballad", "Jazz Ballad", "Jazz", "4/4 swing", 4, 12, 0, 66,
     "S: g.gg.gg.gg.g\nP: ...x.....x..\nK: g.....g.....",
     "D: x..x.xx..x.x\nP: ...x.....x..\nK: g.....g.....\nS: g..g..g..g..",
     nullptr, nullptr, nullptr, nullptr},
    // ---- Latin
    {"bossa", "Bossa Nova", "Latin", "4/4", 4, 16, 0, 132,
     "K: x..xx..xx..xx..x x..xx..xx..xx..x\nR: x..x..x...x..x.. ..x..x...x..x...\nH: x.x.x.x.x.x.x.x. x.x.x.x.x.x.x.x.",
     "K: x..xx..xx..xx..x x..xx..xx..xx..x\nR: x..x..x...x..x.. ..x..x...x..x...\nD: x.x.x.x.x.x.x.x. x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"samba", "Samba", "Latin", "4/4", 4, 16, 0, 100,
     "K: x..xx..xx..xx..x\nH: XxxxXxxxXxxxXxxx\nR: x..x..x...x.x...",
     "K: x..xx..xx..xx..x\nH: XxxxXxxxXxxxXxxx\nS: x..x..x...x.x...\nCB: x.x..x.x.x..x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"chacha", "Cha-Cha", "Latin", "4/4", 4, 16, 0, 116,
     "CB: x...x...x...x...\nK: x.......x.......\nR: ....x.......x.x.\nGU: x...x...x...x...",
     "CB: x...x...x...x...\nK: x.......x.......\nQH: ......x.x.....x.\nQL: ............x...\nR: ....x.......x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"reggae", "Reggae One Drop", "Reggae", "4/4", 4, 16, 0.35, 76,
     "K: ........x.......\nR: ........x.......\nH: x.x.x.x.x.x.x.x.",
     "K: ........x.......\nS: ........x.......\nH: x.x.x.x.x.x.x.x.\nO: ..............x.",
     nullptr, nullptr, nullptr, nullptr},
    {"ska", "Ska", "Reggae", "4/4", 4, 16, 0, 152,
     "K: x.......x.......\nS: ....x.......x...\nH: ..x...x...x...x.",
     "K: x...x...x...x...\nS: ....x.......x...\nO: ..x...x...x...x.",
     nullptr, nullptr, nullptr, nullptr},
    // ---- three
    {"waltz", "Waltz", "Three", "3/4", 3, 12, 0, 104,
     "K: x...........\nS: ....x...x...\nH: x.x.x.x.x.x.",
     "K: x...........\nS: ....x...x...\nD: x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"sixeight", "6/8 Ballad", "Three", "6/8", 3, 12, 0, 90,
     "K: x...........\nR: ......x.....\nH: x.x.x.x.x.x.",
     "K: x.......x...\nS: ......x.....\nD: x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, nullptr},
    {"twelveeight", "12/8 Slow Blues", "Blues", "12/8", 4, 12, 0, 60,
     "K: x.....x.....\nS: ...x.....x..\nH: xxxxxxxxxxxx",
     "K: x.....x.....\nS: ...x.....x..\nD: xxxxxxxxxxxx",
     nullptr, nullptr, nullptr, nullptr},
    // ---- just time
    {"click", "Simple Time", "Practice", "4/4", 4, 16, 0, 100,
     "R: X...x...x...x...",
     "R: X...x...x...x...\nK: x.......x.......",
     "R: X...x...x...x...", nullptr, nullptr, nullptr},

    // ---- signature grooves: well-known feels, under plain names
    {"heartland", "Heartland Jangle", "Signature", "4/4", 4, 16, 0, 116,
     "K: x.....x.x.......\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.\nTB: ....x.......x...",
     "K: x.....x.x.....x.\nS: ....x.......x...\nO: x.x.x.x.x.x.x.x.\nTB: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "Straight 8ths with a tambourine riding along: heartland rock."},
    {"swungstrum", "Swung Strum-Along", "Signature", "4/4", 4, 16, 0.4, 87,
     "K: x..x..x...x..x..\nS: ....x.......x...\nH: XoxoXoxoXoxoXoxo",
     "K: x..x..x...x..x..\nS: ....x.......x..g\nD: x.x.x.x.x.x.x.x.\nTB: ..x...x...x...x.",
     nullptr, nullptr, nullptr, "A swaggering, swung 16th strum-along groove."},
    {"anthemballad", "Anthem Ballad", "Signature", "4/4", 4, 16, 0, 82,
     "K: x.......x.x.....\nS: ....x.......x...\nD: x.x.x.x.x.x.x.x.",
     "K: x.......x.x.....\nS: ....x.......x...\nO: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "A big, steady singalong ballad beat. Lighters up."},
    {"flowing16", "Flowing 16ths", "Signature", "4/4", 4, 16, 0, 100,
     "K: x..x..x...x.....\nS: ....x..g.g..x...\nH: xoxoxoxoxoxoxoxo",
     "K: x..x..x...x..x..\nS: ....x..g....x...\nO: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "Funky 16th hats that just keep rolling."},
    {"loudquiet", "Loud-Quiet-Loud", "Signature", "4/4", 4, 16, 0, 117,
     "K: x.......x.x.....\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.....x.x.x.....\nS: ....X.......X...\nY: x.x.x.x.x.x.x.x.",
     "K: x...............\nS: ....x...XxXxXxXx", nullptr, nullptr, "Quiet verse, LOUD chorus with crash 8ths."},
    {"pulse332", "3-3-2 Pulse", "Signature", "4/4", 4, 16, 0, 96,
     "K: x..x..x.x..x..x.\nCL: ....x.......x...\nH: o.o.o.o.o.o.o.o.",
     "K: x..x..x.x..x..x.\nS: ....x.......x...\nCL: ....x.......x...\nSH: xoxoxoxoxoxoxoxo",
     nullptr, nullptr, nullptr, "The 3-3-2 dancehall-pop pulse loop pedals love."},
    {"slowsway", "Slow Sway", "Signature", "4/4 swing", 4, 12, 0, 79,
     "K: x.....x..x..\nR: ...x.....x..\nH: x.xx.xx.xx.x",
     "K: x.....x..x..\nS: ...x.....x..\nD: x.xx.xx.xx.x",
     nullptr, nullptr, nullptr, "A slow, swaying wedding-dance groove."},
    {"pubstomp", "Pub Stomp", "Signature", "4/4", 4, 16, 0, 99,
     "K: x...x...x...x...\nCL: ....x.......x...\nTB: ..x...x...x...x.",
     "K: x...x...x...x...\nS: ....x.......x...\nCL: ....x.......x...\nTB: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "A pub stomp with claps. Fiddle not included."},
    {"tomdrive", "Floor Tom Drive", "Signature", "4/4", 4, 16, 0, 135,
     "K: x.x.x.x.x.x.x.x.\nF: x...x...x...x...",
     "K: x.x.x.x.x.x.x.x.\nS: ....x.......x...\nY: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "Driving 8ths on the kick and floor tom, then it all opens up."},
    {"junglebeat", "Jungle Beat", "Signature", "4/4", 4, 16, 0, 104,
     "F: x..x..x...x.x...\nSH: xoxoxoxoxoxoxoxo\nK: x.......x.......",
     "F: x..x..x...x.x...\nSH: xoxoxoxoxoxoxoxo\nK: x.......x.......\nS: ....x.......x...",
     nullptr, nullptr, nullptr, "The shave-and-a-haircut 3-2 beat on toms and maracas."},
    {"stompclap", "Stomp Stomp Clap", "Signature", "4/4", 4, 16, 0, 81,
     "K: x.x.....x.x.....\nCL: ....x.......x...",
     "K: x.x.....x.x.....\nCL: ....X.......X...\nY: x...............",
     "K: x.x.....x.x.....\nCL: ....x...x.x.x.x.", nullptr, nullptr, "Stomp, stomp, clap. The whole room can do it."},
    {"stairwell", "Stairwell Stomp", "Signature", "4/4", 4, 16, 0.35, 72,
     "K: x..xx.....xx....\nS: ....X.......X...\nH: x.x.x.x.x.x.x.x.",
     "K: x..xx.....xx....\nS: ....X.......X...\nO: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "A huge, heavy half-time stomp with a big room around it."},
    {"reggaerock", "Reggae-Rock Trio", "Signature", "4/4", 4, 16, 0.25, 118,
     "K: ........x.......\nS: ........X.......\nH: x.x.x.x.x.x.x.x.\nR: ..x.........x...",
     "K: x.......x.......\nS: ....x.......x...\nD: x.x.x.x.x.x.x.x.\nB: ..x...x...x...x.",
     nullptr, nullptr, nullptr, "One drop in the verse, straight in the chorus."},
    {"ghostshuffle", "Ghost-Note Shuffle", "Signature", "4/4 shuffle", 4, 12, 0, 86,
     "K: x.....x....x\nS: .g.gXg.g.gXg\nH: x.xx.xx.xx.x",
     "K: x.....x....x\nS: .g.gXg.g.gXg\nO: x.xx.xx.xx.x",
     nullptr, nullptr, nullptr, "A ghost-noted half-time shuffle. Practice your ghosts."},
    {"deadstraight", "Dead Straight", "Signature", "4/4", 4, 16, 0, 94,
     "K: x.......x.......\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.......x.......\nS: ....x.......x...\nO: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "Dead simple, dead straight, dead loud. Never a note more."},
    {"boomchicka", "Boom-Chicka", "Signature", "4/4", 4, 16, 0, 106,
     "K: x...o...x...o...\nS: g.X.g.X.g.X.g.X.",
     "K: x...o...x...o...\nS: g.X.g.X.g.X.g.X.\nH: ..x...x...x...x.",
     nullptr, nullptr, nullptr, "Boom-chicka-boom train beat for a fast country two-step."},
    {"tight80s", "Tight '80s Pop", "Signature", "4/4", 4, 16, 0, 117,
     "K: x.......x.......\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.\nSH: xoxoxoxoxoxoxoxo",
     "K: x.......x.......\nS: ....x.......x...\nCL: ....x.......x...\nH: x.x.x.x.x.x.x.x.\nSH: xoxoxoxoxoxoxoxo",
     nullptr, nullptr, nullptr, "That tight, never-changing '80s pop groove."},
    {"clavfunk", "Clav Funk", "Signature", "4/4", 4, 16, 0, 100,
     "K: x..x..x...x.....\nS: ..g.x..g.g..x..g\nH: XoxoXoxoXoxoXo.o\nO: ..............x.",
     "K: x..x..x...x..x..\nS: ..g.x..g.g..x..g\nH: XoxoXoxoXoxoXo.o\nO: ..............x.\nCB: x...x...x...x...",
     nullptr, nullptr, nullptr, "Funk 16ths with an open hat to finish the bar."},
    {"tommarch", "Floor Tom March", "Signature", "4/4", 4, 16, 0, 124,
     "K: x...x...x...x...\nF: x...x...x...x...",
     "K: x...x...x...x...\nS: ....x.......x...\nY: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "A thudding floor-tom march for a big crowd."},
    {"banjostomp", "Banjo Stomp", "Signature", "4/4", 4, 16, 0, 138,
     "K: x...x...x...x...\nTB: ..x...x...x...x.",
     "K: x...x...x...x...\nS: ....x.......x...\nTB: ..x...x...x...x.\nY: x...............",
     nullptr, nullptr, nullptr, "Four-on-the-floor folk stomp, banjos optional."},
    {"discorock", "Disco-Rock 16ths", "Signature", "4/4", 4, 16, 0, 148,
     "K: x...x...x...x...\nS: ....x.......x...\nH: xxxxxxxxxxxxxxxx",
     "K: x...x...x...x...\nS: ....x.......x...\nO: ..x...x...x...x.\nH: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "Breathless disco-rock 16ths."},
    {"poppunk", "Pop-Punk", "Signature", "4/4", 4, 16, 0, 176,
     "K: x.x...x.x.x.....\nS: ....x.......x...\nH: x.x.x.x.x.x.x.x.",
     "K: x.x...x.x.x.....\nS: ....x.......x...\nY: x.x.x.x.x.x.x.x.",
     nullptr, nullptr, nullptr, "Fast pop-punk, tight and loud."},
    {"dreamy16", "Dreamy 16ths", "Signature", "4/4", 4, 16, 0, 120,
     "K: x.......x.......\nS: ....x.......x...\nH: xoxoxoxoxoxoxoxo",
     "K: x.......x..x....\nS: ....x.......x...\nH: xoxoxoxoxoxoxoxo\nO: ..............x.",
     nullptr, nullptr, nullptr, "Laid-back, dreamy 16ths."},
    {"sevenfour", "Seven Four (7/4)", "Signature", "7/4", 7, 28, 0, 120,
     "K: x.......x.......x...........\nS: ....x.......x.......x...x...\nH: x.x.x.x.x.x.x.x.x.x.x.x.x.x.",
     "K: x.......x.......x...........\nS: ....x.......x.......x...x...\nD: x.x.x.x.x.x.x.x.x.x.x.x.x.x.",
     "K: x...........................\nS: ....x.......x...xxxxxxxxxxxx", nullptr, nullptr, "Seven beats to the bar."},
    {"fivefour", "Five Four (5/4)", "Signature", "5/4", 5, 20, 0, 120,
     "K: x.....x...x.....x...\nS: ....x.......x.......\nCL: ....x.....x.x...x...\nH: x.x.x.x.x.x.x.x.x.x.",
     "K: x.....x...x.....x...\nS: ....x.......x.......\nCL: ....x.....x.x...x...\nO: x.x.x.x.x.x.x.x.x.x.",
     "K: x...................\nS: ....x...x...xxxxxxxx", nullptr, nullptr, "Five beats to the bar, with claps."},
};

Part fallback_fill(int quarters, int steps) {
    if (steps == 12 && quarters == 4) return parse(FILL12, steps);
    if (quarters == 3) return parse(FILL34, steps);
    return parse(FILL16, steps);
}

// a one-bar count on the hi-hat (the drummer's "1, 2, 3, 4")
Part count_in(int quarters, int steps) {
    Part p;
    int per = steps / quarters;
    for (int q = 0; q < quarters; q++) p.hits.push_back({q * per, 42, q == 0 ? 120 : 96});
    return p;
}

}  // namespace

const char *part_label(int s) {
    static const char *N[] = {"Verse", "Chorus", "Fill to chorus", "Fill to verse", "Count-in", "Ending", "Clap along"};
    return s >= 0 && s < SECTIONS ? N[s] : "";
}
const char *part_key(int s) {
    static const char *N[] = {"verse", "chorus", "fill", "fill-to-verse", "intro", "ending", "clap"};
    return s >= 0 && s < SECTIONS ? N[s] : "";
}
int part_by_key(const std::string &key) {
    for (int s = 0; s < SECTIONS; s++)
        if (key == part_key(s)) return s;
    return -1;
}

Part default_clap(const std::string &meter, int quarters, int steps) {
    Part p;
    int per = steps / std::max(1, quarters);
    if (meter.rfind("6/8", 0) == 0) p.hits.push_back({steps / 2, 39, 118});
    else if (quarters == 3) p.hits = {{per, 39, 118}, {2 * per, 39, 110}};
    else
        for (int q = 1; q < quarters; q += 2) p.hits.push_back({q * per, 39, 118});
    return p;
}

bool same_part(const Part &a, const Part &b) {
    if (a.bars != b.bars || a.hits.size() != b.hits.size()) return false;
    for (const auto &h : a.hits) {
        bool found = false;
        for (const auto &g : b.hits)
            if (g.step == h.step && g.note == h.note && g.vel == h.vel) found = true;
        if (!found) return false;
    }
    return true;
}

namespace {

Part ending_hit() {
    Part p;
    p.hits = {{0, 36, 120}, {0, 49, 124}};
    return p;
}

}  // namespace

namespace {
std::mutex g_mu;
std::deque<Style> g_styles;   // a deque: adding beats never moves the ones the engine is playing
std::map<std::string, size_t> g_latest;   // id -> its newest version
std::vector<std::string> g_removed;

Style make(const Def &d) {
    Style s;
    s.id = d.id;
    s.name = d.name;
    s.family = d.family;
    s.meter = d.meter;
    s.quarters = d.quarters;
    s.steps = d.steps;
    s.swing = d.swing;
    s.bpm = d.bpm;
    s.about = d.about ? d.about : "";
    s.parts[VERSE] = parse(d.verse, d.steps);
    s.parts[CHORUS] = d.chorus ? parse(d.chorus, d.steps) : s.parts[VERSE];
    Part fill = d.fill ? parse(d.fill, d.steps) : fallback_fill(d.quarters, d.steps);
    s.parts[FILL_VERSE] = fill;
    s.parts[FILL_CHORUS] = fill;
    s.parts[INTRO] = d.intro ? parse(d.intro, d.steps) : count_in(d.quarters, d.steps);
    s.parts[ENDING] = d.ending ? parse(d.ending, d.steps) : ending_hit();
    s.parts[CLAP] = default_clap(s.meter, s.quarters, s.steps);
    return s;
}

void ensure() {
    if (!g_styles.empty()) return;
    for (const Def &d : DEFS) {
        g_styles.push_back(make(d));
        g_latest[d.id] = g_styles.size() - 1;
    }
}

const Style *add(const Style &s) {
    g_styles.push_back(s);
    g_latest[s.id] = g_styles.size() - 1;
    g_removed.erase(std::remove(g_removed.begin(), g_removed.end(), s.id), g_removed.end());
    return &g_styles.back();
}
}  // namespace

std::vector<const Style *> styles() {
    std::lock_guard<std::mutex> lock(g_mu);
    ensure();
    std::vector<const Style *> out;
    for (size_t i = 0; i < g_styles.size(); i++) {
        const Style &s = g_styles[i];
        if (g_latest[s.id] != i) continue;
        if (std::find(g_removed.begin(), g_removed.end(), s.id) != g_removed.end()) continue;
        if (s.id == "edit-preview") continue;
        out.push_back(&s);
    }
    return out;
}

const Style *find(const std::string &id) {
    std::lock_guard<std::mutex> lock(g_mu);
    ensure();
    auto it = g_latest.find(id);
    if (it == g_latest.end()) return nullptr;
    if (std::find(g_removed.begin(), g_removed.end(), id) != g_removed.end()) return nullptr;
    return &g_styles[it->second];
}

const std::vector<std::pair<std::string, int>> &drum_names() {
    static const std::vector<std::pair<std::string, int>> N = {
        {"Kick", 36},        {"Snare", 38},      {"Side Stick", 37}, {"Rimshot", 40},    {"Clap", 39},
        {"Hi-Hat", 42},      {"Open Hat", 46},   {"Pedal Hat", 44},  {"Ride", 51},       {"Ride Bell", 53},
        {"Crash", 49},       {"Crash 2", 57},    {"High Tom", 48},   {"Mid Tom", 47},    {"Low Tom", 45},
        {"Floor Tom", 43},   {"Tambourine", 54}, {"Shaker", 70},     {"Cowbell", 56},    {"Claves", 75},
        {"Woodblock", 76},   {"Bongo Hi", 60},   {"Bongo Lo", 61},   {"Conga Hi", 63},   {"Conga Lo", 64}};
    return N;
}

static std::string code_of(int note) {
    for (const auto &kv : DRUM)
        if (kv.second == note) return kv.first;
    return std::to_string(note);
}

static char char_of(int vel) { return vel >= 115 ? 'X' : vel >= 88 ? 'x' : vel >= 58 ? 'o' : 'g'; }

std::string lane(const Style &s, int section, int note) {
    const Part &p = s.parts[section];
    std::string out((size_t)(s.steps * std::max(1, p.bars)), '.');
    for (const auto &h : p.hits)
        if (h.note == note && h.step >= 0 && h.step < (int)out.size()) out[(size_t)h.step] = char_of(h.vel);
    return out;
}

std::string to_text(const Style &s) {
    std::ostringstream o;
    o << "# A Barline beat. X accent, x hit, o soft, g ghost, . rest\n";
    o << "name = " << s.name << "\nmeter = " << s.meter << "\nquarters = " << s.quarters << "\nsteps = " << s.steps << "\nswing = " << s.swing
      << "\nbpm = " << s.bpm << "\n";
    // [fill] goes into the chorus, [fill-to-verse] back out of it; [intro] is the count-in
    for (int sec : {VERSE, CHORUS, FILL_VERSE, FILL_CHORUS, INTRO, ENDING, CLAP}) {
        if (sec == FILL_CHORUS && same_part(s.parts[FILL_CHORUS], s.parts[FILL_VERSE])) continue;   // the same fill both ways
        if (sec == CLAP && same_part(s.parts[CLAP], default_clap(s.meter, s.quarters, s.steps))) continue;
        o << "\n[" << part_key(sec) << "]\n";
        if (s.parts[sec].hits.empty()) o << "# (nothing: silence)\n";
        std::vector<int> notes;
        for (const auto &h : s.parts[sec].hits)
            if (std::find(notes.begin(), notes.end(), h.note) == notes.end()) notes.push_back(h.note);
        for (int n : notes) {
            std::string l = lane(s, sec, n), spaced;
            int per = s.steps / std::max(1, s.quarters);
            for (size_t i = 0; i < l.size(); i++) {
                if (i && per && i % (size_t)per == 0) spaced += ' ';
                spaced += l[i];
            }
            o << code_of(n) << ": " << spaced << "\n";
        }
    }
    return o.str();
}

static std::string slugify(const std::string &name) {
    std::string out;
    for (char c : name) {
        if (std::isalnum((unsigned char)c)) out += (char)std::tolower((unsigned char)c);
        else if (!out.empty() && out.back() != '-') out += '-';
    }
    while (!out.empty() && out.back() == '-') out.pop_back();
    return out.empty() ? "beat" : out;
}

bool from_text(const std::string &text, Style &s) {
    std::istringstream f(text);
    std::map<std::string, std::string> sec;
    std::string cur, line;
    while (std::getline(f, line)) {
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!line.empty() && line[0] == '#') continue;
        if (line.size() > 2 && line[0] == '[') {
            cur = line.substr(1, line.find(']') - 1);
            sec[cur];   // there, even if it's empty
            continue;
        }
        if (cur.empty()) {
            size_t eq = line.find('=');
            if (eq == std::string::npos) continue;
            std::string k = line.substr(0, eq), v = line.substr(eq + 1);
            while (!k.empty() && k.back() == ' ') k.pop_back();
            while (!v.empty() && v[0] == ' ') v.erase(0, 1);
            if (k == "name") s.name = v.substr(0, 60);
            else if (k == "meter") s.meter = v.substr(0, 20);
            else if (k == "quarters") s.quarters = std::max(1, std::min(16, std::atoi(v.c_str())));
            else if (k == "steps") s.steps = std::max(1, std::min(64, std::atoi(v.c_str())));
            else if (k == "swing") s.swing = std::max(0.0, std::min(1.0, std::atof(v.c_str())));
            else if (k == "bpm") s.bpm = std::max(30, std::min(300, std::atoi(v.c_str())));
        } else
            sec[cur] += line + "\n";
    }
    if (s.name.empty() || sec["verse"].empty()) return false;
    s.parts[VERSE] = parse(sec["verse"], s.steps);
    s.parts[CHORUS] = sec["chorus"].empty() ? s.parts[VERSE] : parse(sec["chorus"], s.steps);
    // a part that's there but empty is silence on purpose (an ending with no hit, say); one that's
    // missing gets the usual
    auto has = [&](const char *k) { return sec.count(k) > 0; };
    Part fill = has("fill") ? parse(sec["fill"], s.steps) : fallback_fill(s.quarters, s.steps);
    s.parts[FILL_VERSE] = fill;
    s.parts[FILL_CHORUS] = has("fill-to-verse") ? parse(sec["fill-to-verse"], s.steps) : fill;
    s.parts[INTRO] = has("intro") ? parse(sec["intro"], s.steps) : count_in(s.quarters, s.steps);
    s.parts[ENDING] = has("ending") ? parse(sec["ending"], s.steps) : ending_hit();
    s.parts[CLAP] = has("clap") ? parse(sec["clap"], s.steps) : default_clap(s.meter, s.quarters, s.steps);
    // at most 2 bars a part
    for (auto &p : s.parts) {
        p.bars = std::min(p.bars, 2);
        p.hits.erase(std::remove_if(p.hits.begin(), p.hits.end(), [&](const Hit &h) { return h.step >= s.steps * 2; }), p.hits.end());
    }
    return true;
}

static bool read_beat(const std::string &path, Style &s) {
    std::ifstream f(path);
    if (!f) return false;
    std::ostringstream o;
    o << f.rdbuf();
    return from_text(o.str(), s);
}

void load_user(const std::string &dir) {
    std::vector<std::string> names;
    for (const auto &n : sys::list_dir(dir))
        if (n.size() > 5 && n.compare(n.size() - 5, 5, ".beat") == 0) names.push_back(n);
    std::sort(names.begin(), names.end());
    std::lock_guard<std::mutex> lock(g_mu);
    ensure();
    for (const auto &n : names) {
        Style s;
        if (!read_beat(dir + "/" + n, s)) continue;
        s.id = "my-" + n.substr(0, n.size() - 5);
        s.family = "My Beats";
        s.user = true;
        add(s);
    }
}

const Style *save_user(const std::string &dir, const Style &in, std::string *err) {
    Style s = in;
    s.user = true;
    s.family = "My Beats";
    if (s.id.compare(0, 3, "my-") != 0) {
        // a new beat: a file name from its name, not taking an existing one
        std::string base = slugify(s.name), id = "my-" + base;
        for (int k = 2; find(id) && find(id)->user; k++) id = "my-" + base + "-" + std::to_string(k);
        s.id = id;
    }
    sys::make_dir(dir, 0755);
    std::string path = dir + "/" + s.id.substr(3) + ".beat";
    if (!sys::write_private(path, to_text(s))) {
        if (err) *err = "couldn't save " + path;
        return nullptr;
    }
    std::lock_guard<std::mutex> lock(g_mu);
    ensure();
    return add(s);
}

const Style *preview(const Style &in) {
    Style s = in;
    s.id = "edit-preview";
    std::lock_guard<std::mutex> lock(g_mu);
    ensure();
    return add(s);
}

bool delete_user(const std::string &dir, const std::string &id) {
    if (id.compare(0, 3, "my-") != 0) return false;
    std::string path = dir + "/" + id.substr(3) + ".beat";
    std::remove(path.c_str());
    std::lock_guard<std::mutex> lock(g_mu);
    g_removed.push_back(id);
    return true;
}

}  // namespace pattern
