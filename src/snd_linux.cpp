// Linux: sound and MIDI through the kernel's own interface (/dev/snd), no libraries.
#ifdef __linux__
#include "snd.h"

#include <algorithm>
#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <initializer_list>
#include <sstream>

#include <dirent.h>
#include <fcntl.h>
#include <poll.h>
#include <sound/asound.h>
#include <sys/ioctl.h>
#include <unistd.h>

namespace snd {

std::vector<Card> cards() {
    std::vector<Card> out;
    std::ifstream f("/proc/asound/cards");
    std::string line;
    while (std::getline(f, line)) {
        // " 1 [Looper         ]: USB-Audio - Looper Pedal"
        int idx;
        char id[64] = {0};
        if (std::sscanf(line.c_str(), " %d [%63[^]]", &idx, id) != 2) continue;
        Card c;
        c.index = idx;
        c.id = id;
        while (!c.id.empty() && c.id.back() == ' ') c.id.pop_back();
        size_t dash = line.find(" - ");
        c.name = dash == std::string::npos ? c.id : line.substr(dash + 3);
        out.push_back(c);
    }
    if (DIR *d = opendir("/dev/snd")) {
        while (dirent *e = readdir(d)) {
            int ci, di;
            char kind;
            for (auto &c : out) {
                if (std::sscanf(e->d_name, "pcmC%dD%d%c", &ci, &di, &kind) == 3 && ci == c.index)
                    (kind == 'p' ? c.play : c.record).push_back(di);
                else if (std::sscanf(e->d_name, "midiC%dD%d", &ci, &di) == 2 && ci == c.index)
                    c.midi.push_back(di);
            }
        }
        closedir(d);
    }
    for (auto &c : out) {
        std::sort(c.play.begin(), c.play.end());
        std::sort(c.record.begin(), c.record.end());
        std::sort(c.midi.begin(), c.midi.end());
    }
    return out;
}

// ----------------------------------------------------------------------------- PCM

namespace {

snd_mask *mask_of(snd_pcm_hw_params *p, int n) { return &p->masks[n - SNDRV_PCM_HW_PARAM_FIRST_MASK]; }
snd_interval *iv_of(snd_pcm_hw_params *p, int n) { return &p->intervals[n - SNDRV_PCM_HW_PARAM_FIRST_INTERVAL]; }

void params_init(snd_pcm_hw_params *p) {
    std::memset(p, 0, sizeof *p);
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_MASK; n <= SNDRV_PCM_HW_PARAM_LAST_MASK; n++) {
        snd_mask *m = mask_of(p, n);
        for (auto &b : m->bits) b = ~0u;
    }
    for (int n = SNDRV_PCM_HW_PARAM_FIRST_INTERVAL; n <= SNDRV_PCM_HW_PARAM_LAST_INTERVAL; n++) {
        snd_interval *i = iv_of(p, n);
        i->min = 0;
        i->max = ~0u;
    }
    p->rmask = ~0u;
    p->cmask = 0;
    p->info = ~0u;
}

void set_mask(snd_pcm_hw_params *p, int n, unsigned bit) {
    snd_mask *m = mask_of(p, n);
    for (auto &b : m->bits) b = 0;
    m->bits[bit >> 5] |= 1u << (bit & 31);
}

void set_int(snd_pcm_hw_params *p, int n, unsigned v) {
    snd_interval *i = iv_of(p, n);
    i->min = i->max = v;
    i->integer = 1;
}

void set_range(snd_pcm_hw_params *p, int n, unsigned lo, unsigned hi) {
    snd_interval *i = iv_of(p, n);
    i->min = lo;
    i->max = hi;
    i->integer = 1;
}

unsigned get_int(snd_pcm_hw_params *p, int n) { return iv_of(p, n)->min; }

struct Format {
    int kernel;
    int bytes;
    const char *name;
};
// best quality first; the pedal is a 24-bit device
const Format FORMATS[] = {{SNDRV_PCM_FORMAT_S32_LE, 4, "S32_LE"},
                          {SNDRV_PCM_FORMAT_S24_3LE, 3, "S24_3LE"},
                          {SNDRV_PCM_FORMAT_S24_LE, 4, "S24_LE"},
                          {SNDRV_PCM_FORMAT_S16_LE, 2, "S16_LE"}};

}  // namespace

bool Pcm::open(int card, int device, bool capture, unsigned want_rate, unsigned ch, unsigned want_period, unsigned want_periods,
               std::string *err) {
    close();
    char path[64];
    std::snprintf(path, sizeof path, "/dev/snd/pcmC%dD%d%c", card, device, capture ? 'c' : 'p');
    // opened without waiting: a device another program has would otherwise make open() wait until it lets go
    fd_ = ::open(path, O_RDWR | O_CLOEXEC | O_NONBLOCK);
    if (fd_ < 0) {
        if (err) *err = std::string(path) + ": " + std::strerror(errno) + (errno == EBUSY || errno == EAGAIN ? " (another program is using it)" : "");
        return false;
    }
    fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) & ~O_NONBLOCK);   // then writes wait for room, as they should
    capture_ = capture;
    in_ch_ = ch;
    // try each sample format, and block sizes from the one asked for upward, until the card agrees;
    // then other channel counts (a card with 4 or 6 outputs may only take them all: the sound goes
    // to the first two); last, any block size at all (odd built-in sound, like a T2 Mac's)
    std::vector<unsigned> chans = {ch};
    for (unsigned c : {2u, 4u, 6u, 8u, 1u})
        if (std::find(chans.begin(), chans.end(), c) == chans.end()) chans.push_back(c);
    for (int any_block = 0; any_block < 2; any_block++)
    for (unsigned c : chans)
    for (const Format &f : FORMATS) {
        for (unsigned period = want_period; period <= want_period * 16; period *= 2) {
            for (unsigned np : {want_periods, want_periods + 1, 2u, 3u, 4u}) {
                if (any_block && (period != want_period || np != want_periods)) continue;   // once is enough
                snd_pcm_hw_params p;
                params_init(&p);
                set_mask(&p, SNDRV_PCM_HW_PARAM_ACCESS, SNDRV_PCM_ACCESS_RW_INTERLEAVED);
                set_mask(&p, SNDRV_PCM_HW_PARAM_FORMAT, (unsigned)f.kernel);
                set_mask(&p, SNDRV_PCM_HW_PARAM_SUBFORMAT, SNDRV_PCM_SUBFORMAT_STD);
                set_int(&p, SNDRV_PCM_HW_PARAM_CHANNELS, c);
                set_int(&p, SNDRV_PCM_HW_PARAM_RATE, want_rate);
                if (!any_block) {
                    set_range(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE, period, period);
                    set_int(&p, SNDRV_PCM_HW_PARAM_PERIODS, np);
                }
                if (ioctl(fd_, SNDRV_PCM_IOCTL_HW_PARAMS, &p) < 0) continue;
                rate = get_int(&p, SNDRV_PCM_HW_PARAM_RATE);
                channels = get_int(&p, SNDRV_PCM_HW_PARAM_CHANNELS);
                this->period = get_int(&p, SNDRV_PCM_HW_PARAM_PERIOD_SIZE);
                periods = get_int(&p, SNDRV_PCM_HW_PARAM_PERIODS);
                bytes_ = f.bytes;
                fmt_ = f.kernel;
                format = f.name;
                // software side: start when the buffer is full, wake for each block
                snd_pcm_sw_params sw;
                std::memset(&sw, 0, sizeof sw);
                unsigned long buffer = (unsigned long)this->period * periods;
                sw.tstamp_mode = SNDRV_PCM_TSTAMP_NONE;
                sw.period_step = 1;
                sw.avail_min = this->period;
                sw.start_threshold = capture ? 1 : buffer;
                sw.stop_threshold = buffer;
                unsigned long boundary = buffer;
                while (boundary * 2 <= (unsigned long)LONG_MAX - buffer) boundary *= 2;
                sw.boundary = boundary;
                if (ioctl(fd_, SNDRV_PCM_IOCTL_SW_PARAMS, &sw) < 0) {
                    if (err) *err = std::string("software settings refused: ") + std::strerror(errno);
                    close();
                    return false;
                }
                if (ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE) < 0) {
                    if (err) *err = std::string("couldn't start: ") + std::strerror(errno);
                    close();
                    return false;
                }
                if (capture) ioctl(fd_, SNDRV_PCM_IOCTL_START);
                return true;
            }
        }
    }
    if (err) *err = "the device doesn't take " + std::to_string(want_rate) + " Hz, " + std::to_string(ch) + " channels";
    close();
    return false;
}

void Pcm::close() {
    if (fd_ >= 0) {
        if (!capture_) ioctl(fd_, SNDRV_PCM_IOCTL_DRAIN);
        ::close(fd_);
    }
    fd_ = -1;
}

bool Pcm::recover(int e) {
    if (e == EPIPE || e == ESTRPIPE) {   // ran dry (or a laptop suspend): start again
        xruns++;
        if (ioctl(fd_, SNDRV_PCM_IOCTL_PREPARE) < 0) return false;
        if (capture_) ioctl(fd_, SNDRV_PCM_IOCTL_START);
        return true;
    }
    return e == EINTR || e == EAGAIN;
}

bool Pcm::write(const float *data, unsigned frames) {
    if (fd_ < 0) return false;
    size_t n = (size_t)frames * channels;
    buf_.resize(n * (size_t)bytes_);
    uint8_t *o = buf_.data();
    for (size_t i = 0; i < n; i++) {
        // the device's channel from the ones given: the same where they match, the extra outputs silent,
        // and a mono device gets both sides
        size_t fr = i / channels, c = i % channels;
        float in = channels == in_ch_ ? data[i]
                 : channels == 1 && in_ch_ >= 2 ? 0.5f * (data[fr * in_ch_] + data[fr * in_ch_ + 1])
                 : c < in_ch_ ? data[fr * in_ch_ + c] : 0.0f;
        float v = std::max(-1.0f, std::min(1.0f, in));
        int32_t s = (int32_t)std::lrint(v * 2147483392.0);   // full scale, then cut to the container
        switch (fmt_) {
        case SNDRV_PCM_FORMAT_S16_LE:
            s >>= 16;
            o[0] = (uint8_t)s;
            o[1] = (uint8_t)(s >> 8);
            o += 2;
            break;
        case SNDRV_PCM_FORMAT_S24_3LE:
            s >>= 8;
            o[0] = (uint8_t)s;
            o[1] = (uint8_t)(s >> 8);
            o[2] = (uint8_t)(s >> 16);
            o += 3;
            break;
        case SNDRV_PCM_FORMAT_S24_LE:
            s >>= 8;
            std::memcpy(o, &s, 4);
            o += 4;
            break;
        default:
            std::memcpy(o, &s, 4);
            o += 4;
        }
    }
    unsigned done = 0;
    while (done < frames) {
        snd_xferi x;
        x.result = 0;
        x.buf = buf_.data() + (size_t)done * channels * (size_t)bytes_;
        x.frames = frames - done;
        if (ioctl(fd_, SNDRV_PCM_IOCTL_WRITEI_FRAMES, &x) < 0) {
            if (!recover(errno)) return false;
            continue;
        }
        done += (unsigned)x.result;
    }
    return true;
}

bool Pcm::read(float *data, unsigned frames) {
    if (fd_ < 0) return false;
    size_t n = (size_t)frames * channels;
    buf_.resize(n * (size_t)bytes_);
    unsigned done = 0;
    while (done < frames) {
        snd_xferi x;
        x.result = 0;
        x.buf = buf_.data() + (size_t)done * channels * (size_t)bytes_;
        x.frames = frames - done;
        if (ioctl(fd_, SNDRV_PCM_IOCTL_READI_FRAMES, &x) < 0) {
            if (!recover(errno)) return false;
            continue;
        }
        done += (unsigned)x.result;
    }
    const uint8_t *s = buf_.data();
    for (size_t i = 0; i < n; i++) {
        int32_t v;
        switch (fmt_) {
        case SNDRV_PCM_FORMAT_S16_LE:
            v = (int32_t)(int16_t)(s[0] | (s[1] << 8)) << 16;
            s += 2;
            break;
        case SNDRV_PCM_FORMAT_S24_3LE:
            v = (int32_t)(((uint32_t)s[0] << 8) | ((uint32_t)s[1] << 16) | ((uint32_t)s[2] << 24));
            s += 3;
            break;
        case SNDRV_PCM_FORMAT_S24_LE: {
            int32_t t;
            std::memcpy(&t, s, 4);
            v = (int32_t)((uint32_t)t << 8);
            s += 4;
            break;
        }
        default:
            std::memcpy(&v, s, 4);
            s += 4;
        }
        data[i] = (float)(v / 2147483648.0);
    }
    return true;
}

long Pcm::delay() {
    if (fd_ < 0) return 0;
    snd_pcm_sframes_t d = 0;
    if (ioctl(fd_, SNDRV_PCM_IOCTL_DELAY, &d) < 0) return 0;
    return (long)d;
}

// ----------------------------------------------------------------------------- MIDI

bool Midi::open(int card, int device, bool in, bool out, std::string *err) {
    close();
    char path[64];
    std::snprintf(path, sizeof path, "/dev/snd/midiC%dD%d", card, device);
    int mode = in && out ? O_RDWR : in ? O_RDONLY : O_WRONLY;
    fd_ = ::open(path, mode | O_CLOEXEC | O_NONBLOCK);   // (never wait for a device another program has)
    if (fd_ < 0) {
        if (err) *err = std::string(path) + ": " + std::strerror(errno) + (errno == EBUSY || errno == EAGAIN ? " (another program is using it)" : "");
        return false;
    }
    if (!in) fcntl(fd_, F_SETFL, fcntl(fd_, F_GETFL) & ~O_NONBLOCK);
    return true;
}

void Midi::close() {
    if (fd_ >= 0) ::close(fd_);
    fd_ = -1;
}

bool Midi::send(const uint8_t *b, size_t n) {
    if (fd_ < 0) return false;
    while (n) {
        ssize_t w = ::write(fd_, b, n);
        if (w < 0) {
            if (errno == EINTR) continue;
            if (errno == EAGAIN) {
                pollfd p{fd_, POLLOUT, 0};
                ::poll(&p, 1, 5);
                continue;
            }
            return false;
        }
        b += w;
        n -= (size_t)w;
    }
    return true;
}

bool Midi::send(std::initializer_list<uint8_t> b) { return send(b.begin(), b.size()); }

int Midi::read(uint8_t *buf, size_t n) {
    if (fd_ < 0) return -1;
    ssize_t r = ::read(fd_, buf, n);
    if (r < 0) return (errno == EAGAIN || errno == EINTR) ? 0 : -1;
    return (int)r;
}


}  // namespace snd

#endif  // __linux__
