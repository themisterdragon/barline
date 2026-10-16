// Sound and MIDI, the same on every system: Linux through the kernel's own interface (/dev/snd, no
// libraries: one static program for any Linux), Windows through WASAPI and its MIDI API, macOS through
// Core Audio and Core MIDI. A "card" is one device: the pedal is one card with sound and MIDI.
#pragma once
#include <cstddef>
#include <cstdint>
#include <initializer_list>
#include <string>
#include <vector>

namespace snd {

struct Card {
    int index = -1;
    std::string id, name;            // "Looper", "Looper Pedal"
    std::vector<int> play, record;   // PCM device numbers
    std::vector<int> midi;           // raw MIDI device numbers
};
std::vector<Card> cards();

// PCM audio: float samples in, whatever the card takes out (16/24/32-bit)
class Pcm {
public:
    ~Pcm() { close(); }
    // period: frames per block (the latency is about period x periods)
    bool open(int card, int device, bool capture, unsigned rate, unsigned channels, unsigned period, unsigned periods, std::string *err);
    void close();
    bool is_open() const { return fd_ >= 0; }
    // interleaved floats (-1..1), `frames` frames; waits for room. false on a hard error (unplugged)
    bool write(const float *data, unsigned frames);
    bool read(float *data, unsigned frames);
    long delay();   // frames between what's written now and the speaker
    unsigned rate = 0, channels = 0, period = 0, periods = 0;
    std::string format;   // "S16_LE", "S24_3LE", ...
    int xruns = 0;        // underruns/overruns recovered from

private:
    int fd_ = -1;
    bool capture_ = false;
    int bytes_ = 2;        // bytes per sample in the container
    int fmt_ = 0;          // the kernel's format number
    unsigned in_ch_ = 2;   // the channels Barline gives (the device may have more, or one)
    std::vector<uint8_t> buf_;
    bool recover(int e);
    void *impl_ = nullptr;   // Windows / macOS: the system's own stream
};

// Raw MIDI: bytes in and out
class Midi {
public:
    ~Midi() { close(); }
    bool open(int card, int device, bool in, bool out, std::string *err);
    void close();
    bool is_open() const { return fd_ >= 0; }
    bool send(const uint8_t *b, size_t n);
    bool send(std::initializer_list<uint8_t> b);
    int read(uint8_t *buf, size_t n);   // what's waiting (never blocks); -1 when the device went away
    int fd() const { return fd_; }

private:
    int fd_ = -1;
    void *impl_ = nullptr;   // Windows / macOS: the system's own MIDI port
};

}  // namespace snd
