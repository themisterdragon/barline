#include "kit.h"
#include "sys.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <functional>
#include <map>
#include <random>
#include <sstream>

#ifndef M_PI
#define M_PI 3.14159265358979323846
#endif


namespace kit {

size_t Kit::bytes() const {
    size_t n = 0;
    for (const auto &p : pieces)
        for (const auto &l : p.layers)
            for (const auto &s : l.rr) n += s->pcm.size() * 2;
    return n;
}

// ----------------------------------------------------------------------------- WAV files

static uint32_t le32(const uint8_t *p) { return p[0] | (p[1] << 8) | (p[2] << 16) | ((uint32_t)p[3] << 24); }
static uint16_t le16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

bool read_wav(const std::string &path, Sample &out, std::string *err) {
    std::ifstream f(path, std::ios::binary);
    std::string d((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    const uint8_t *b = (const uint8_t *)d.data();
    if (d.size() < 44 || std::memcmp(b, "RIFF", 4) || std::memcmp(b + 8, "WAVE", 4)) {
        if (err) *err = path + ": not a WAV file";
        return false;
    }
    int ch = 0, bits = 0, fmt = 0;
    size_t pos = 12;
    while (pos + 8 <= d.size()) {
        uint32_t len = le32(b + pos + 4);
        const uint8_t *c = b + pos + 8;
        if (!std::memcmp(b + pos, "fmt ", 4) && len >= 16) {
            fmt = le16(c);
            ch = le16(c + 2);
            bits = le16(c + 14);
        } else if (!std::memcmp(b + pos, "data", 4)) {
            if (fmt != 1 || (bits != 16 && bits != 24) || (ch != 1 && ch != 2)) {
                if (err) *err = path + ": only 16/24-bit PCM WAVs";
                return false;
            }
            size_t bytes = std::min<size_t>(len, d.size() - pos - 8);
            size_t frames = bytes / (size_t)(ch * bits / 8);
            out.pcm.resize(frames * 2);
            for (size_t i = 0; i < frames; i++)
                for (int k = 0; k < 2; k++) {
                    int src = ch == 2 ? k : 0;
                    const uint8_t *s = c + (i * (size_t)ch + (size_t)src) * (size_t)(bits / 8);
                    out.pcm[i * 2 + (size_t)k] = bits == 16 ? (int16_t)le16(s) : (int16_t)((s[1]) | (s[2] << 8));
                }
            return true;
        }
        pos += 8 + len + (len & 1);
    }
    if (err) *err = path + ": no audio in it";
    return false;
}

bool write_wav(const std::string &path, const std::vector<float> &st, int rate) {
    std::ofstream f(path, std::ios::binary);
    if (!f) return false;
    uint32_t n = (uint32_t)(st.size() * 2);
    auto w32 = [&](uint32_t v) { f.put((char)(v & 255)).put((char)((v >> 8) & 255)).put((char)((v >> 16) & 255)).put((char)(v >> 24)); };
    auto w16 = [&](uint16_t v) { f.put((char)(v & 255)).put((char)(v >> 8)); };
    f.write("RIFF", 4);
    w32(36 + n);
    f.write("WAVEfmt ", 8);
    w32(16);
    w16(1);
    w16(2);
    w32((uint32_t)rate);
    w32((uint32_t)rate * 4);
    w16(4);
    w16(16);
    f.write("data", 4);
    w32(n);
    for (float v : st) w16((uint16_t)(int16_t)std::lrint(std::max(-1.0f, std::min(1.0f, v)) * 32767));
    return (bool)f;
}

// ----------------------------------------------------------------------------- electronic kits

namespace {

struct Synth {
    std::mt19937 rng;
    explicit Synth(unsigned seed) : rng(seed) {}
    double noise() { return std::uniform_real_distribution<double>(-1, 1)(rng); }
};

// a two-pole filter (RBJ biquad): band-pass, high-pass, low-pass
struct Biquad {
    double b0 = 1, b1 = 0, b2 = 0, a1 = 0, a2 = 0, z1 = 0, z2 = 0;
    static Biquad make(char kind, double f, double q) {
        Biquad o;
        double w = 2 * M_PI * f / RATE, c = std::cos(w), a = std::sin(w) / (2 * q), a0 = 1 + a;
        if (kind == 'b') {
            o.b0 = a / a0;
            o.b1 = 0;
            o.b2 = -a / a0;
        } else if (kind == 'h') {
            o.b0 = (1 + c) / 2 / a0;
            o.b1 = -(1 + c) / a0;
            o.b2 = (1 + c) / 2 / a0;
        } else {
            o.b0 = (1 - c) / 2 / a0;
            o.b1 = (1 - c) / a0;
            o.b2 = (1 - c) / 2 / a0;
        }
        o.a1 = -2 * c / a0;
        o.a2 = (1 - a) / a0;
        return o;
    }
    double run(double x) {
        double y = b0 * x + z1;
        z1 = b1 * x - a1 * y + z2;
        z2 = b2 * x - a2 * y;
        return y;
    }
};

using Voice = std::function<double(Synth &, double t, double v)>;   // t seconds, v 0..1 loudness

std::shared_ptr<Sample> render(const std::function<std::vector<double>(Synth &, double)> &make, unsigned seed, double v, double width = 0) {
    Synth s(seed);
    std::vector<double> m = make(s, v);
    auto out = std::make_shared<Sample>();
    out->pcm.resize(m.size() * 2);
    // a touch of stereo: a few samples' delay on one side for the bright sounds
    int d = (int)(width * 0.0004 * RATE);
    for (size_t i = 0; i < m.size(); i++) {
        double l = m[i], r = i >= (size_t)d ? m[i - (size_t)d] : 0;
        out->pcm[i * 2] = (int16_t)std::lrint(std::max(-1.0, std::min(1.0, l)) * 32000);
        out->pcm[i * 2 + 1] = (int16_t)std::lrint(std::max(-1.0, std::min(1.0, r)) * 32000);
    }
    return out;
}

std::vector<double> buffer(double secs) { return std::vector<double>((size_t)(secs * RATE), 0.0); }

// the six detuned square waves of the classic analog cymbals and hats
double metal(double t, double scale) {
    static const double F[6] = {205.3, 304.4, 369.6, 522.7, 540.0, 800.0};
    double s = 0;
    for (double f : F) s += std::sin(2 * M_PI * f * scale * t) > 0 ? 1 : -1;
    return s / 6;
}

struct Recipe {
    const char *slug, *name, *about;
    double kick_from, kick_to, kick_decay, kick_drive;
    double snare_tone, snare_noise, snare_decay;
    double hat_bright;
    double crush;   // 0 = clean; else the lo-fi sample rate
};

const Recipe RECIPES[] = {
    {"boom8", "Boom 8 (analog)", "Synthesized: deep boomy kick, soft snare, classic analog hats and cowbell.", 110, 48, 0.75, 0.0, 0.55, 0.45,
     0.17, 1.0, 0},
    {"club9", "Club 9 (analog)", "Synthesized: punchy driven kick, bright snare and claps; dance and pop.", 230, 52, 0.32, 0.6, 0.35, 0.75, 0.2,
     1.4, 0},
    {"lofi", "Lo-Fi Tape", "Synthesized and crunched: dusty, muffled beat for chill and hip-hop.", 140, 50, 0.45, 0.3, 0.5, 0.6, 0.18, 0.8,
     11025},
};

double crush(std::vector<double> &a, double rate) {
    if (rate <= 0) return 0;
    // hold each sample (lower sample rate) and round to 8 bits, then soften the top
    double step = RATE / rate, held = 0, ph = step;
    Biquad lp = Biquad::make('l', rate * 0.45, 0.7);
    for (auto &x : a) {
        if (ph >= step) {
            held = std::round(x * 127) / 127;
            ph -= step;
        }
        ph += 1;
        x = lp.run(held);
    }
    return 0;
}

std::shared_ptr<Kit> make_synth(const Recipe &R) {
    auto k = std::make_shared<Kit>();
    k->slug = R.slug;
    k->name = R.name;
    k->by = "Barline";
    k->license = "Barline's own sounds";
    k->style = "electronic";
    k->about = R.about;
    // each drum: a function of (synth, loudness 0..1) giving mono samples
    std::map<int, std::pair<std::string, std::function<std::vector<double>(Synth &, double)>>> drums;
    drums[36] = {"Kick", [&](Synth &s, double v) {
                     auto a = buffer(R.kick_decay * 3);
                     double ph = 0;
                     for (size_t i = 0; i < a.size(); i++) {
                         double t = (double)i / RATE;
                         double f = R.kick_to + (R.kick_from - R.kick_to) * std::exp(-t / 0.035);
                         ph += 2 * M_PI * f / RATE;
                         double x = std::sin(ph) * std::exp(-t / R.kick_decay);
                         x += (t < 0.004 ? s.noise() * (1 - t / 0.004) * 0.3 * v : 0);   // the beater click
                         if (R.kick_drive > 0) x = std::tanh(x * (1 + 3 * R.kick_drive)) / std::tanh(1 + 3 * R.kick_drive);
                         a[i] = x * (0.35 + 0.65 * v);
                     }
                     return a;
                 }};
    drums[38] = {"Snare", [&](Synth &s, double v) {
                     auto a = buffer(R.snare_decay * 4);
                     Biquad hp = Biquad::make('h', 1800 + 1500 * v, 0.7);
                     for (size_t i = 0; i < a.size(); i++) {
                         double t = (double)i / RATE;
                         double tone = (std::sin(2 * M_PI * 185 * t) + 0.6 * std::sin(2 * M_PI * 330 * t)) * std::exp(-t / 0.06) * R.snare_tone;
                         double nz = hp.run(s.noise()) * std::exp(-t / R.snare_decay) * R.snare_noise * 1.6;
                         a[i] = (tone + nz) * (0.3 + 0.7 * v);
                     }
                     return a;
                 }};
    drums[40] = drums[38];
    drums[40].first = "Rimshot";
    drums[39] = {"Clap", [&](Synth &s, double v) {
                     auto a = buffer(0.45);
                     Biquad bp = Biquad::make('b', 1150, 1.6);
                     for (size_t i = 0; i < a.size(); i++) {
                         double t = (double)i / RATE;
                         // a few quick hands, then the room
                         double e = 0;
                         for (int h = 0; h < 4; h++) {
                             double th = t - h * 0.011;
                             if (th >= 0) e = std::max(e, std::exp(-th / (h == 3 ? 0.14 : 0.006)));
                         }
                         a[i] = bp.run(s.noise()) * e * 2.4 * (0.3 + 0.7 * v);
                     }
                     return a;
                 }};
    drums[37] = {"Side Stick", [&](Synth &s, double v) {
                     auto a = buffer(0.12);
                     Biquad bp = Biquad::make('b', 1700, 4);
                     for (size_t i = 0; i < a.size(); i++) {
                         double t = (double)i / RATE;
                         a[i] = (std::sin(2 * M_PI * 1700 * t) * 0.6 + bp.run(s.noise()) * 1.5) * std::exp(-t / 0.012) * (0.3 + 0.7 * v);
                     }
                     return a;
                 }};
    auto hat = [&](double decay, double level) {
        return [&, decay, level](Synth &s, double v) {
            auto a = buffer(decay * 5 + 0.02);
            Biquad bp = Biquad::make('b', 9500 * R.hat_bright, 0.9), hp = Biquad::make('h', 7000 * R.hat_bright, 0.7);
            for (size_t i = 0; i < a.size(); i++) {
                double t = (double)i / RATE;
                double x = hp.run(bp.run(metal(t, 1.0) + 0.3 * s.noise()));
                a[i] = x * std::exp(-t / decay) * level * 2.2 * (0.35 + 0.65 * v);
            }
            return a;
        };
    };
    drums[42] = {"Hi-Hat Closed", hat(0.035, 1.0)};
    drums[44] = {"Hi-Hat Pedal", hat(0.022, 0.7)};
    drums[46] = {"Hi-Hat Open", hat(0.32, 0.9)};
    auto tom = [&](double f0) {
        return [&, f0](Synth &s, double v) {
            auto a = buffer(1.0);
            double ph = 0;
            for (size_t i = 0; i < a.size(); i++) {
                double t = (double)i / RATE;
                double f = f0 * (1 + 0.5 * std::exp(-t / 0.05));
                ph += 2 * M_PI * f / RATE;
                a[i] = (std::sin(ph) + (t < 0.003 ? s.noise() * 0.2 : 0)) * std::exp(-t / 0.22) * (0.35 + 0.65 * v);
            }
            return a;
        };
    };
    drums[43] = {"Floor Tom", tom(82)};
    drums[45] = {"Low Tom", tom(110)};
    drums[47] = {"Mid Tom", tom(140)};
    drums[48] = {"High Tom", tom(180)};
    auto cymbal = [&](double decay, double scale, double bell) {
        return [&, decay, scale, bell](Synth &s, double v) {
            auto a = buffer(decay * 4);
            Biquad hp = Biquad::make('h', 5000 * R.hat_bright, 0.6);
            for (size_t i = 0; i < a.size(); i++) {
                double t = (double)i / RATE;
                double x = hp.run(metal(t, scale) * 0.7 + s.noise() * 0.5) * std::exp(-t / decay);
                x += bell * (std::sin(2 * M_PI * 1250 * t) + 0.5 * std::sin(2 * M_PI * 1875 * t)) * std::exp(-t / 0.5);
                a[i] = x * 1.4 * (0.35 + 0.65 * v);
            }
            return a;
        };
    };
    drums[49] = {"Crash", cymbal(0.9, 1.7, 0)};
    drums[57] = {"Crash 2", cymbal(1.1, 1.45, 0)};
    drums[51] = {"Ride", cymbal(0.7, 2.3, 0.08)};
    drums[53] = {"Ride Bell", cymbal(0.6, 2.3, 0.35)};
    drums[56] = {"Cowbell", [&](Synth &, double v) {
                     auto a = buffer(0.5);
                     Biquad bp = Biquad::make('b', 2600, 1.2);
                     for (size_t i = 0; i < a.size(); i++) {
                         double t = (double)i / RATE;
                         double x = (std::sin(2 * M_PI * 540 * t) > 0 ? 1 : -1) + (std::sin(2 * M_PI * 800 * t) > 0 ? 1 : -1);
                         a[i] = (bp.run(x) * 0.8 + x * 0.15) * std::exp(-t / 0.09) * (0.35 + 0.65 * v);
                     }
                     return a;
                 }};
    drums[75] = {"Claves", [&](Synth &, double v) {
                     auto a = buffer(0.2);
                     for (size_t i = 0; i < a.size(); i++) {
                         double t = (double)i / RATE;
                         a[i] = std::sin(2 * M_PI * 2500 * t) * std::exp(-t / 0.035) * (0.35 + 0.65 * v);
                     }
                     return a;
                 }};
    drums[70] = {"Shaker", [&](Synth &s, double v) {
                     auto a = buffer(0.12);
                     Biquad hp = Biquad::make('h', 6000, 0.7);
                     for (size_t i = 0; i < a.size(); i++) {
                         double t = (double)i / RATE;
                         double e = t < 0.02 ? t / 0.02 : std::exp(-(t - 0.02) / 0.03);
                         a[i] = hp.run(s.noise()) * e * 0.8 * (0.35 + 0.65 * v);
                     }
                     return a;
                 }};
    unsigned seed = 1;
    for (auto &d : drums) {
        Piece p;
        p.note = d.first;
        p.name = d.second.first;
        if (p.note == 42 || p.note == 44 || p.note == 46) p.choke = 0;
        for (int L = 0; L < 3; L++) {   // three loudness layers, two round-robins each
            Layer layer;
            layer.lo = L == 0 ? 1 : L == 1 ? 50 : 96;
            layer.hi = L == 0 ? 49 : L == 1 ? 95 : 127;
            double v = L == 0 ? 0.35 : L == 1 ? 0.7 : 1.0;
            for (int r = 0; r < 2; r++) {
                auto make = [&](Synth &s, double vv) {
                    auto a = d.second.second(s, vv);
                    if (R.crush > 0) crush(a, R.crush);
                    return a;
                };
                bool bright = p.note == 42 || p.note == 44 || p.note == 46 || p.note >= 49;
                layer.rr.push_back(render(make, seed++, v, bright ? 1 : 0));
            }
            p.layers.push_back(layer);
        }
        k->pieces.push_back(p);
    }
    for (size_t i = 0; i < k->pieces.size(); i++) k->index[(size_t)k->pieces[i].note] = (int)i + 1;
    k->gain = 0.9;
    return k;
}

}  // namespace

const std::vector<Info> &synth_kits() {
    static std::vector<Info> out;
    if (out.empty())
        for (const auto &r : RECIPES) {
            Info i;
            i.slug = r.slug;
            i.name = r.name;
            i.style = "electronic";
            i.by = "Barline";
            i.license = "Barline's own sounds";
            i.about = r.about;
            i.synth = true;
            out.push_back(i);
        }
    return out;
}

// ----------------------------------------------------------------------------- kit folders

static std::string field(const std::string &line, const std::string &key) {
    if (line.compare(0, key.size() + 1, key + " ") == 0) return line.substr(key.size() + 1);
    return "";
}

std::vector<Info> list(const std::string &dir) {
    std::vector<Info> out;
    {
        for (const auto &name : sys::list_dir(dir)) {
            if (name[0] == '.') continue;
            std::ifstream f(dir + "/" + name + "/kit.txt");
            if (!f) continue;
            Info i;
            i.slug = name;
            for (std::string line; std::getline(f, line);) {
                if (!field(line, "name").empty()) i.name = field(line, "name");
                if (!field(line, "style").empty()) i.style = field(line, "style");
                if (!field(line, "by").empty()) i.by = field(line, "by");
                if (!field(line, "license").empty()) i.license = field(line, "license");
                if (!field(line, "about").empty()) i.about = field(line, "about");
                if (line.compare(0, 6, "piece ") == 0) break;
            }
            out.push_back(i);
        }
    }
    std::sort(out.begin(), out.end(), [](const Info &a, const Info &b) { return a.name < b.name; });
    for (const auto &s : synth_kits()) out.push_back(s);
    return out;
}

// a piece from another kit, made as loud next to this kit as it was in its own
static void borrow(Kit &k, const Kit &from, int note) {
    const Piece *p = from.piece(note);
    if (!p || k.piece(note)) return;
    Piece copy = *p;
    copy.choke = -1;
    double g = from.gain / k.gain;
    for (auto &L : copy.layers)
        for (auto &rr : L.rr) {
            auto s = std::make_shared<Sample>(*rr);
            for (auto &x : s->pcm) x = (int16_t)std::max(-32768.0, std::min(32767.0, x * g));
            rr = s;
        }
    k.pieces.push_back(copy);
    k.index[(size_t)note] = (int)k.pieces.size();
}

static std::shared_ptr<Kit> load_kit(const std::string &dir, const std::string &slug, std::string *err, int only = -1);

std::shared_ptr<Kit> load(const std::string &dir, const std::string &slug, std::string *err) {
    auto k = load_kit(dir, slug, err);
    // the clap-along needs hand claps: a kit without them borrows the Cajon kit's real ones (or
    // Barline's own, made like a drum machine's)
    if (k && !k->piece(39)) {
        auto c = load_kit(dir, "cajon", nullptr, 39);
        if (!c || !c->piece(39)) c = load_kit(dir, RECIPES[0].slug, nullptr);
        if (c) borrow(*k, *c, 39);
    }
    return k;
}

static std::shared_ptr<Kit> load_kit(const std::string &dir, const std::string &slug, std::string *err, int only) {
    for (const auto &r : RECIPES)
        if (slug == r.slug) return make_synth(r);
    std::string folder = dir + "/" + slug;
    std::ifstream f(folder + "/kit.txt");
    if (!f) {
        if (err) *err = "no kit called " + slug;
        return nullptr;
    }
    auto k = std::make_shared<Kit>();
    k->slug = slug;
    double loudness = -20;
    std::vector<int> choke;
    for (std::string line; std::getline(f, line);) {
        std::istringstream in(line);
        std::string key;
        in >> key;
        std::string rest = line.size() > key.size() + 1 ? line.substr(key.size() + 1) : "";
        if (key == "name") k->name = rest;
        else if (key == "by") k->by = rest;
        else if (key == "license") k->license = rest;
        else if (key == "url") k->url = rest;
        else if (key == "style") k->style = rest;
        else if (key == "about") k->about = rest;
        else if (key == "loudness") loudness = std::atof(rest.c_str());
        else if (key == "choke") {
            for (int n; in >> n;) choke.push_back(n);
        } else if (key == "piece") {
            Piece p;
            in >> p.note;
            std::getline(in, p.name);
            if (!p.name.empty() && p.name[0] == ' ') p.name.erase(0, 1);
            if (p.note < 0 || p.note > 127) continue;
            k->pieces.push_back(p);
            k->index[(size_t)p.note] = (int)k->pieces.size();
        } else if (key == "layer") {
            int note;
            Layer L;
            in >> note >> L.lo >> L.hi;
            Piece *p = note >= 0 && note < 128 && k->index[(size_t)note] ? &k->pieces[(size_t)k->index[(size_t)note] - 1] : nullptr;
            if (!p || (only >= 0 && note != only)) continue;   // (only one drum wanted: the others aren't read)
            for (std::string file; in >> file;) {
                auto s = std::make_shared<Sample>();
                std::string e;
                if (file.find('/') != std::string::npos || !read_wav(folder + "/" + file, *s, &e)) {
                    if (err) *err = e;
                    continue;
                }
                L.rr.push_back(s);
            }
            if (!L.rr.empty()) p->layers.push_back(L);
        }
    }
    for (int n : choke)
        if (n >= 0 && n < 128 && k->index[(size_t)n]) k->pieces[(size_t)k->index[(size_t)n] - 1].choke = 0;
    // even the kits out: every kit's snare about as loud (-20 dB)
    k->gain = std::pow(10.0, (-20.0 - loudness) / 20.0);
    if (k->pieces.empty()) {
        if (err && err->empty()) *err = slug + ": the kit is empty";
        return nullptr;
    }
    return k;
}

int stand_in(const Kit &k, int note) {
    static const std::map<int, std::vector<int>> CHAINS = {
        {35, {36}},         {36, {35}},          {37, {40, 38}},      {38, {40, 37}},     {39, {38, 40}},         {40, {38, 37}},
        {41, {43, 45}},     {43, {41, 45}},      {45, {47, 43}},      {47, {45, 48}},     {48, {50, 47, 45}},     {50, {48, 47}},
        {42, {44, 70, 54}}, {44, {42, 70}},      {46, {42, 70, 54}},  {49, {57, 55, 52, 51, 46}}, {57, {49, 55, 52}},
        {55, {49, 57}},     {52, {49, 57}},      {51, {59, 53, 42}},  {59, {51, 53}},     {53, {51, 56}},         {54, {70, 46, 42}},
        {70, {54, 42}},     {69, {70, 54, 42}},  {56, {53, 75}},      {75, {37, 76}},     {76, {75, 37}},         {77, {76, 75}},
        {60, {48, 50}},     {61, {45, 47}},      {62, {48}},          {63, {48}},         {64, {45}},             {73, {70}}};
    if (k.piece(note)) return note;
    auto it = CHAINS.find(note);
    if (it == CHAINS.end()) return -1;
    for (int n : it->second)
        if (k.piece(n)) return n;
    return -1;
}

}  // namespace kit
