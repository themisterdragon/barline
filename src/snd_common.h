// What the Windows and macOS sound code share: their systems list sound outputs and MIDI ports
// separately, so a device's sound and its MIDI are put back together into one "card" by name
// (the pedal shows up as "Speakers (Looper Pedal)" and "Looper Pedal").
#pragma once
#include <algorithm>
#include <atomic>
#include <cctype>
#include <cstdint>
#include <string>
#include <vector>

#include "snd.h"

namespace snd {
namespace common {

inline std::string lower(std::string s) {
    for (auto &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

// a short id for a name: letters and digits only ("LooperPedal")
inline std::string short_id(const std::string &name) {
    std::string id;
    for (char c : name)
        if (std::isalnum((unsigned char)c) && id.size() < 16) id += c;
    return id.empty() ? "Device" : id;
}

// The sound outputs (by position in `outs`) and MIDI ports (by position in `midis`) as cards: a MIDI
// port goes with the output whose name contains its name (or the other way round); the rest are
// cards of their own. card.play / card.midi hold the positions.
inline std::vector<Card> merge(const std::vector<std::string> &outs, const std::vector<std::string> &midis) {
    std::vector<Card> cards;
    for (size_t i = 0; i < outs.size(); i++) {
        Card c;
        c.index = (int)cards.size();
        c.name = outs[i];
        c.id = short_id(outs[i]);
        c.play.push_back((int)i);
        cards.push_back(c);
    }
    for (size_t m = 0; m < midis.size(); m++) {
        std::string mn = lower(midis[m]);
        Card *home = nullptr;
        for (auto &c : cards) {
            std::string cn = lower(c.name);
            if (!mn.empty() && c.midi.empty() && (cn.find(mn) != std::string::npos || mn.find(cn) != std::string::npos)) {
                home = &c;
                break;
            }
        }
        if (!home) {
            Card c;
            c.index = (int)cards.size();
            c.name = midis[m];
            c.id = short_id(midis[m]);
            cards.push_back(c);
            home = &cards.back();
        }
        home->midi.push_back((int)m);
    }
    return cards;
}

// how many bytes a MIDI message with this status byte has (0: not one Barline sends)
inline int message_length(uint8_t status) {
    if (status >= 0x80 && status < 0xC0) return 3;
    if (status >= 0xC0 && status < 0xE0) return 2;
    if (status >= 0xE0 && status < 0xF0) return 3;
    if (status == 0xF2) return 3;
    if (status == 0xF1 || status == 0xF3) return 2;
    if (status == 0xF6 || status >= 0xF8) return 1;
    return 0;
}

// a little ring of floats between the engine (writing) and the system's sound thread (reading):
// one writer, one reader, no locks
struct Ring {
    std::vector<float> buf;   // interleaved stereo
    size_t frames = 0;
    std::atomic<uint64_t> wrote{0}, read{0};
    void init(size_t f) {
        frames = f;
        buf.assign(f * 2, 0.0f);
        wrote = read = 0;
    }
    size_t filled() const { return (size_t)(wrote.load() - read.load()); }
    size_t room() const { return frames - filled(); }
    void push(const float *in, size_t n) {   // n <= room()
        uint64_t w = wrote.load();
        for (size_t i = 0; i < n; i++) {
            size_t at = (size_t)((w + i) % frames) * 2;
            buf[at] = in[i * 2];
            buf[at + 1] = in[i * 2 + 1];
        }
        wrote.store(w + n);
    }
    // what's there, the rest silence; returns frames that were really there
    size_t pull(float *out, size_t n, unsigned out_ch) {
        uint64_t r = read.load();
        size_t have = std::min(n, (size_t)(wrote.load() - r));
        for (size_t i = 0; i < n; i++) {
            float l = 0, rr = 0;
            if (i < have) {
                size_t at = (size_t)((r + i) % frames) * 2;
                l = buf[at];
                rr = buf[at + 1];
            }
            for (unsigned c = 0; c < out_ch; c++) out[i * out_ch + c] = c == 0 ? l : c == 1 ? rr : 0.0f;
        }
        read.store(r + have);
        return have;
    }
};

}  // namespace common
}  // namespace snd
