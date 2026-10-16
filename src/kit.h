// Drum kits: sampled ones (a folder with kit.txt and 16-bit WAVs, built by tools/kits) and the
// electronic ones Barline synthesizes itself. Every kit puts its drums on General MIDI notes.
#pragma once
#include <array>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace kit {

const int RATE = 44100;

struct Sample {
    std::vector<int16_t> pcm;   // stereo, interleaved
    uint32_t frames() const { return (uint32_t)(pcm.size() / 2); }
};

struct Layer {
    int lo = 1, hi = 127;   // velocities
    std::vector<std::shared_ptr<Sample>> rr;   // round-robins
};

struct Piece {
    int note = 0;
    std::string name;
    std::vector<Layer> layers;
    int choke = -1;   // hits in the same group cut each other off (open/closed hi-hat)
};

struct Kit {
    std::string slug, name, by, license, url, style, about;
    double gain = 1;   // evens kits out so switching doesn't jump in volume
    std::vector<Piece> pieces;
    std::array<int, 128> index{};   // note -> piece + 1 (0 = none)
    const Piece *piece(int note) const { return note >= 0 && note < 128 && index[(size_t)note] ? &pieces[(size_t)index[(size_t)note] - 1] : nullptr; }
    size_t bytes() const;
};

// the kit folders under dir (each with a kit.txt); (slug, name, style)
struct Info {
    std::string slug, name, style, by, license, about;
    bool synth = false;
};
std::vector<Info> list(const std::string &dir);
// a kit by slug: from dir, or one of the synthesized ones; nullptr (and err) when it can't load
std::shared_ptr<Kit> load(const std::string &dir, const std::string &slug, std::string *err = nullptr);
const std::vector<Info> &synth_kits();

// a note a kit doesn't have, played on one it does (a cajon kit has no ride: use its shaker)
int stand_in(const Kit &k, int note);

bool read_wav(const std::string &path, Sample &out, std::string *err = nullptr);
bool write_wav(const std::string &path, const std::vector<float> &stereo, int rate = RATE);

}  // namespace kit
