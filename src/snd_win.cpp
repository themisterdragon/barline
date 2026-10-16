// Windows: sound through WASAPI (shared mode: Windows converts the rate and channels, so any output
// works, the pedal included, with Windows' own USB audio driver), MIDI through the Windows MIDI API.
#ifdef _WIN32
#include "snd.h"

#include <windows.h>
#include <mmdeviceapi.h>
#include <audioclient.h>
#include <mmsystem.h>

#include <cstring>
#include <deque>
#include <mutex>

#include "snd_common.h"

namespace snd {

namespace {

const PROPERTYKEY KEY_FRIENDLY_NAME = {{0xa45c254e, 0xdf1c, 0x4efd, {0x80, 0x20, 0x67, 0xd1, 0x46, 0xa8, 0x50, 0xe0}}, 14};
const CLSID CLSID_Enumerator = {0xbcde0395, 0xe52f, 0x467c, {0x8e, 0x3d, 0xc4, 0x57, 0x92, 0x91, 0x69, 0x2e}};
const IID IID_Enumerator = {0xa95664d2, 0x9614, 0x4f35, {0xa7, 0x46, 0xde, 0x8d, 0xb6, 0x36, 0x17, 0xe6}};
const IID IID_Client = {0x1cb9ad4c, 0xdbfa, 0x4c32, {0xb1, 0x78, 0xc2, 0xf5, 0x68, 0xa7, 0x03, 0xb2}};
const IID IID_Render = {0xf294acfc, 0x3146, 0x4483, {0xa7, 0xbf, 0xad, 0xdc, 0xa7, 0xc2, 0x60, 0xe2}};
const GUID FLOAT_SUBTYPE = {0x00000003, 0x0000, 0x0010, {0x80, 0x00, 0x00, 0xaa, 0x00, 0x38, 0x9b, 0x71}};

// COM, once on each thread that talks to WASAPI
void com() {
    thread_local bool done = false;
    if (!done) {
        CoInitializeEx(nullptr, COINIT_MULTITHREADED);
        done = true;
    }
}

std::string utf8(const wchar_t *w) {
    if (!w) return "";
    int n = WideCharToMultiByte(CP_UTF8, 0, w, -1, nullptr, 0, nullptr, nullptr);
    if (n <= 1) return "";
    std::string s((size_t)n - 1, '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, -1, &s[0], n, nullptr, nullptr);
    return s;
}

// the devices as the last cards() saw them: a card's play/midi numbers point in here
std::mutex g_mu;
std::vector<std::wstring> g_outs;   // render endpoint ids
std::vector<UINT> g_midi_outs;      // Windows MIDI output numbers

struct Stream {
    IMMDevice *dev = nullptr;
    IAudioClient *client = nullptr;
    IAudioRenderClient *render = nullptr;
    HANDLE event = nullptr;
    UINT32 frames = 0;     // the buffer's size
    long latency = 0;      // frames the system adds after the buffer
    bool started = false;
    void release() {
        if (client && started) client->Stop();
        if (render) render->Release();
        if (client) client->Release();
        if (dev) dev->Release();
        if (event) CloseHandle(event);
        *this = Stream();
    }
};

struct MidiPort {
    HMIDIOUT out = nullptr;
    HMIDIIN in = nullptr;
    std::mutex mu;
    std::deque<uint8_t> got;   // what came in, waiting to be read
};

void CALLBACK on_midi_in(HMIDIIN, UINT msg, DWORD_PTR user, DWORD_PTR p1, DWORD_PTR) {
    if (msg != MIM_DATA) return;
    auto *m = (MidiPort *)user;
    uint8_t st = (uint8_t)(p1 & 0xFF);
    int n = common::message_length(st);
    std::lock_guard<std::mutex> lock(m->mu);
    for (int i = 0; i < std::max(1, n); i++) m->got.push_back((uint8_t)((p1 >> (8 * i)) & 0xFF));
    while (m->got.size() > 4096) m->got.pop_front();
}

}  // namespace

std::vector<Card> cards() {
    com();
    std::vector<std::string> out_names, midi_names;
    std::vector<std::wstring> outs;
    std::vector<UINT> midis;
    IMMDeviceEnumerator *en = nullptr;
    if (SUCCEEDED(CoCreateInstance(CLSID_Enumerator, nullptr, CLSCTX_ALL, IID_Enumerator, (void **)&en))) {
        IMMDeviceCollection *col = nullptr;
        if (SUCCEEDED(en->EnumAudioEndpoints(eRender, DEVICE_STATE_ACTIVE, &col))) {
            UINT n = 0;
            col->GetCount(&n);
            for (UINT i = 0; i < n; i++) {
                IMMDevice *d = nullptr;
                if (FAILED(col->Item(i, &d))) continue;
                LPWSTR id = nullptr;
                IPropertyStore *ps = nullptr;
                std::string name;
                if (SUCCEEDED(d->OpenPropertyStore(STGM_READ, &ps))) {
                    PROPVARIANT v;
                    PropVariantInit(&v);
                    if (SUCCEEDED(ps->GetValue(KEY_FRIENDLY_NAME, &v)) && v.vt == VT_LPWSTR) name = utf8(v.pwszVal);
                    PropVariantClear(&v);
                    ps->Release();
                }
                if (SUCCEEDED(d->GetId(&id))) {
                    outs.push_back(id);
                    out_names.push_back(name.empty() ? "Sound output" : name);
                    CoTaskMemFree(id);
                }
                d->Release();
            }
            col->Release();
        }
        en->Release();
    }
    UINT nm = midiOutGetNumDevs();
    for (UINT i = 0; i < nm; i++) {
        MIDIOUTCAPSW caps;
        if (midiOutGetDevCapsW(i, &caps, sizeof caps) != MMSYSERR_NOERROR) continue;
        if (caps.wTechnology == MOD_SWSYNTH) continue;   // Windows' own GS Wavetable synth: not a device
        midis.push_back(i);
        midi_names.push_back(utf8(caps.szPname));
    }
    std::lock_guard<std::mutex> lock(g_mu);
    g_outs = outs;
    g_midi_outs = midis;
    return common::merge(out_names, midi_names);
}

bool Pcm::open(int card, int device, bool capture, unsigned want_rate, unsigned ch, unsigned want_period, unsigned want_periods,
               std::string *err) {
    close();
    (void)card;
    if (capture) {
        if (err) *err = "recording isn't supported here";
        return false;
    }
    com();
    std::wstring id;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (device < 0 || device >= (int)g_outs.size()) {
            if (err) *err = "that sound output isn't there";
            return false;
        }
        id = g_outs[(size_t)device];
    }
    auto *s = new Stream();
    auto fail = [&](const char *what) {
        s->release();
        delete s;
        if (err) *err = what;
        return false;
    };
    IMMDeviceEnumerator *en = nullptr;
    if (FAILED(CoCreateInstance(CLSID_Enumerator, nullptr, CLSCTX_ALL, IID_Enumerator, (void **)&en))) return fail("Windows' sound system didn't answer");
    HRESULT hr = en->GetDevice(id.c_str(), &s->dev);
    en->Release();
    if (FAILED(hr)) return fail("that sound output went away");
    if (FAILED(s->dev->Activate(IID_Client, CLSCTX_ALL, nullptr, (void **)&s->client))) return fail("couldn't open that sound output");
    // float stereo at the rate asked for; Windows converts to whatever the device runs at
    WAVEFORMATEXTENSIBLE f{};
    f.Format.wFormatTag = WAVE_FORMAT_EXTENSIBLE;
    f.Format.nChannels = (WORD)ch;
    f.Format.nSamplesPerSec = want_rate;
    f.Format.wBitsPerSample = 32;
    f.Format.nBlockAlign = (WORD)(ch * 4);
    f.Format.nAvgBytesPerSec = want_rate * ch * 4;
    f.Format.cbSize = sizeof f - sizeof f.Format;
    f.Samples.wValidBitsPerSample = 32;
    f.dwChannelMask = ch == 1 ? SPEAKER_FRONT_CENTER : SPEAKER_FRONT_LEFT | SPEAKER_FRONT_RIGHT;
    f.SubFormat = FLOAT_SUBTYPE;
    // the buffer: the blocks asked for, but never under 20 ms (shared mode's own floor is about 10)
    double want_ms = std::max(20.0, 1000.0 * want_period * std::max(2u, want_periods) / want_rate);
    REFERENCE_TIME dur = (REFERENCE_TIME)(want_ms * 10000);
    DWORD flags = AUDCLNT_STREAMFLAGS_EVENTCALLBACK | 0x80000000 /*AUTOCONVERTPCM*/ | 0x08000000 /*SRC_DEFAULT_QUALITY*/;
    hr = s->client->Initialize(AUDCLNT_SHAREMODE_SHARED, flags, dur, 0, (WAVEFORMATEX *)&f, nullptr);
    if (hr == AUDCLNT_E_DEVICE_IN_USE) return fail("another program has it to itself (exclusive mode)");
    if (FAILED(hr)) return fail("Windows wouldn't play to it at that rate");
    s->event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    s->client->SetEventHandle(s->event);
    s->client->GetBufferSize(&s->frames);
    REFERENCE_TIME lat = 0;
    if (SUCCEEDED(s->client->GetStreamLatency(&lat))) s->latency = (long)(lat * (double)want_rate / 1e7);
    if (FAILED(s->client->GetService(IID_Render, (void **)&s->render))) return fail("couldn't play to it");
    impl_ = s;
    fd_ = 0;
    rate = want_rate;
    channels = ch;
    in_ch_ = ch;
    period = std::min(want_period, s->frames / 2);
    periods = std::max(2u, s->frames / std::max(1u, period));
    format = "float (Windows shared mode)";
    return true;
}

void Pcm::close() {
    if (auto *s = (Stream *)impl_) {
        s->release();
        delete s;
    }
    impl_ = nullptr;
    fd_ = -1;
}

bool Pcm::recover(int) { return false; }

bool Pcm::write(const float *data, unsigned frames) {
    auto *s = (Stream *)impl_;
    if (!s) return false;
    com();
    unsigned done = 0;
    while (done < frames) {
        UINT32 pad = 0;
        HRESULT hr = s->client->GetCurrentPadding(&pad);
        if (FAILED(hr)) return false;   // unplugged (AUDCLNT_E_DEVICE_INVALIDATED)
        UINT32 room = s->frames - pad;
        if (room == 0) {
            if (!s->started) {   // full before it started: start it
                s->client->Start();
                s->started = true;
                continue;
            }
            WaitForSingleObject(s->event, 50);
            continue;
        }
        UINT32 n = std::min(room, frames - done);
        BYTE *buf = nullptr;
        if (FAILED(s->render->GetBuffer(n, &buf))) return false;
        std::memcpy(buf, data + (size_t)done * channels, (size_t)n * channels * sizeof(float));
        if (FAILED(s->render->ReleaseBuffer(n, 0))) return false;
        done += n;
        if (!s->started) {
            s->client->Start();
            s->started = true;
        }
    }
    return true;
}

bool Pcm::read(float *, unsigned) { return false; }

long Pcm::delay() {
    auto *s = (Stream *)impl_;
    if (!s) return 0;
    UINT32 pad = 0;
    if (FAILED(s->client->GetCurrentPadding(&pad))) return 0;
    return (long)pad + s->latency;
}

// ----------------------------------------------------------------------------- MIDI

bool Midi::open(int card, int device, bool in, bool out, std::string *err) {
    close();
    (void)card;
    UINT id;
    std::string name;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (device < 0 || device >= (int)g_midi_outs.size()) {
            if (err) *err = "that MIDI port isn't there";
            return false;
        }
        id = g_midi_outs[(size_t)device];
    }
    MIDIOUTCAPSW caps;
    if (midiOutGetDevCapsW(id, &caps, sizeof caps) == MMSYSERR_NOERROR) name = utf8(caps.szPname);
    auto *m = new MidiPort();
    if (out && midiOutOpen(&m->out, id, 0, 0, CALLBACK_NULL) != MMSYSERR_NOERROR) {
        delete m;
        if (err) *err = "another program has it (Windows lets one program use a MIDI port at a time)";
        return false;
    }
    if (in) {   // the input with the same name
        UINT ni = midiInGetNumDevs();
        for (UINT i = 0; i < ni; i++) {
            MIDIINCAPSW ic;
            if (midiInGetDevCapsW(i, &ic, sizeof ic) != MMSYSERR_NOERROR || utf8(ic.szPname) != name) continue;
            if (midiInOpen(&m->in, i, (DWORD_PTR)&on_midi_in, (DWORD_PTR)m, CALLBACK_FUNCTION) == MMSYSERR_NOERROR) midiInStart(m->in);
            break;
        }
    }
    impl_ = m;
    fd_ = 0;
    return true;
}

void Midi::close() {
    if (auto *m = (MidiPort *)impl_) {
        if (m->in) {
            midiInStop(m->in);
            midiInClose(m->in);
        }
        if (m->out) {
            midiOutReset(m->out);
            midiOutClose(m->out);
        }
        delete m;
    }
    impl_ = nullptr;
    fd_ = -1;
}

bool Midi::send(const uint8_t *b, size_t n) {
    auto *m = (MidiPort *)impl_;
    if (!m || !m->out) return false;
    // one message at a time (a bank select and a program change can come together)
    size_t i = 0;
    while (i < n) {
        int len = common::message_length(b[i]);
        if (len == 0 || i + (size_t)len > n) {
            i++;
            continue;
        }
        DWORD msg = 0;
        for (int k = 0; k < len; k++) msg |= (DWORD)b[i + (size_t)k] << (8 * k);
        if (midiOutShortMsg(m->out, msg) != MMSYSERR_NOERROR) return false;   // unplugged
        i += (size_t)len;
    }
    return true;
}

bool Midi::send(std::initializer_list<uint8_t> b) { return send(b.begin(), b.size()); }

int Midi::read(uint8_t *buf, size_t n) {
    auto *m = (MidiPort *)impl_;
    if (!m) return -1;
    std::lock_guard<std::mutex> lock(m->mu);
    size_t k = 0;
    while (k < n && !m->got.empty()) {
        buf[k++] = m->got.front();
        m->got.pop_front();
    }
    return (int)k;
}

}  // namespace snd

#endif  // _WIN32
