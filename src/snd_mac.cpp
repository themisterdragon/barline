// macOS: sound through Core Audio (an output unit on the device picked; it converts the rate, so
// any output works), MIDI through Core MIDI. The pedal is class compliant: no driver to install.
#ifdef __APPLE__
#include "snd.h"

#include <AudioToolbox/AudioToolbox.h>
#include <CoreAudio/CoreAudio.h>
#include <CoreMIDI/CoreMIDI.h>

#include <chrono>
#include <cstring>
#include <deque>
#include <mutex>
#include <thread>

#include "snd_common.h"

namespace snd {

namespace {

std::string cf_string(CFStringRef s) {
    if (!s) return "";
    char b[512];
    if (CFStringGetCString(s, b, sizeof b, kCFStringEncodingUTF8)) return b;
    return "";
}

// the devices as the last cards() saw them: a card's play/midi numbers point in here
std::mutex g_mu;
std::vector<AudioObjectID> g_outs;
std::vector<MIDIEndpointRef> g_midi_outs;

MIDIClientRef midi_client() {
    static MIDIClientRef c = 0;
    static std::once_flag once;
    std::call_once(once, [] {
        CFStringRef name = CFStringCreateWithCString(nullptr, "Barline", kCFStringEncodingUTF8);
        MIDIClientCreate(name, nullptr, nullptr, &c);
        CFRelease(name);
    });
    return c;
}

UInt32 output_channels(AudioObjectID dev) {
    AudioObjectPropertyAddress a = {kAudioDevicePropertyStreamConfiguration, kAudioObjectPropertyScopeOutput, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(dev, &a, 0, nullptr, &size) != noErr || size == 0) return 0;
    std::vector<uint8_t> buf(size);
    auto *list = (AudioBufferList *)buf.data();
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, list) != noErr) return 0;
    UInt32 ch = 0;
    for (UInt32 i = 0; i < list->mNumberBuffers; i++) ch += list->mBuffers[i].mNumberChannels;
    return ch;
}

UInt32 device_u32(AudioObjectID dev, AudioObjectPropertySelector sel, AudioObjectPropertyScope scope) {
    AudioObjectPropertyAddress a = {sel, scope, kAudioObjectPropertyElementMain};
    UInt32 v = 0, size = sizeof v;
    if (AudioObjectGetPropertyData(dev, &a, 0, nullptr, &size, &v) != noErr) return 0;
    return v;
}

struct Stream {
    AudioComponentInstance unit = nullptr;
    AudioObjectID dev = 0;
    common::Ring ring;
    unsigned ch = 2;
    std::atomic<bool> dead{false};
    long device_latency = 0;   // frames (at Barline's rate) after the ring: the device's own buffer and latency
};

OSStatus render(void *user, AudioUnitRenderActionFlags *, const AudioTimeStamp *, UInt32, UInt32 frames, AudioBufferList *io) {
    auto *s = (Stream *)user;
    float *out = (float *)io->mBuffers[0].mData;
    s->ring.pull(out, frames, s->ch);
    return noErr;
}

OSStatus on_alive(AudioObjectID dev, UInt32, const AudioObjectPropertyAddress *, void *user) {
    auto *s = (Stream *)user;
    if (!device_u32(dev, kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal)) s->dead = true;
    return noErr;
}

struct MidiPort {
    MIDIPortRef out = 0, in = 0;
    MIDIEndpointRef dest = 0, src = 0;
    std::mutex mu;
    std::deque<uint8_t> got;
};

void on_midi_in(const MIDIPacketList *list, void *user, void *) {
    auto *m = (MidiPort *)user;
    std::lock_guard<std::mutex> lock(m->mu);
    const MIDIPacket *p = &list->packet[0];
    for (UInt32 i = 0; i < list->numPackets; i++) {
        for (UInt16 k = 0; k < p->length; k++) m->got.push_back(p->data[k]);
        p = MIDIPacketNext(p);
    }
    while (m->got.size() > 4096) m->got.pop_front();
}

}  // namespace

std::vector<Card> cards() {
    std::vector<std::string> out_names, midi_names;
    std::vector<AudioObjectID> outs;
    std::vector<MIDIEndpointRef> midis;
    AudioObjectPropertyAddress a = {kAudioHardwarePropertyDevices, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    UInt32 size = 0;
    if (AudioObjectGetPropertyDataSize(kAudioObjectSystemObject, &a, 0, nullptr, &size) == noErr && size) {
        std::vector<AudioObjectID> ids(size / sizeof(AudioObjectID));
        if (AudioObjectGetPropertyData(kAudioObjectSystemObject, &a, 0, nullptr, &size, ids.data()) == noErr) {
            for (AudioObjectID id : ids) {
                if (output_channels(id) == 0) continue;
                AudioObjectPropertyAddress na = {kAudioObjectPropertyName, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
                CFStringRef name = nullptr;
                UInt32 ns = sizeof name;
                std::string n;
                if (AudioObjectGetPropertyData(id, &na, 0, nullptr, &ns, &name) == noErr && name) {
                    n = cf_string(name);
                    CFRelease(name);
                }
                outs.push_back(id);
                out_names.push_back(n.empty() ? "Sound output" : n);
            }
        }
    }
    ItemCount nd = MIDIGetNumberOfDestinations();
    for (ItemCount i = 0; i < nd; i++) {
        MIDIEndpointRef e = MIDIGetDestination(i);
        if (!e) continue;
        CFStringRef name = nullptr;
        std::string n;
        if (MIDIObjectGetStringProperty(e, kMIDIPropertyDisplayName, &name) == noErr && name) {
            n = cf_string(name);
            CFRelease(name);
        }
        if (n.rfind("IAC", 0) == 0) continue;   // the Mac's own inter-app buses: not devices
        midis.push_back(e);
        midi_names.push_back(n);
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
    AudioObjectID dev;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (device < 0 || device >= (int)g_outs.size()) {
            if (err) *err = "that sound output isn't there";
            return false;
        }
        dev = g_outs[(size_t)device];
    }
    auto *s = new Stream();
    s->dev = dev;
    s->ch = ch;
    auto fail = [&](const char *what) {
        if (s->unit) AudioComponentInstanceDispose(s->unit);
        delete s;
        if (err) *err = what;
        return false;
    };
    AudioComponentDescription d = {kAudioUnitType_Output, kAudioUnitSubType_HALOutput, kAudioUnitManufacturer_Apple, 0, 0};
    AudioComponent comp = AudioComponentFindNext(nullptr, &d);
    if (!comp || AudioComponentInstanceNew(comp, &s->unit) != noErr) return fail("Core Audio didn't answer");
    if (AudioUnitSetProperty(s->unit, kAudioOutputUnitProperty_CurrentDevice, kAudioUnitScope_Global, 0, &dev, sizeof dev) != noErr)
        return fail("couldn't pick that sound output");
    // the device's own block size: small, for a steady clock
    UInt32 bs = want_period;
    AudioObjectPropertyAddress ba = {kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectSetPropertyData(dev, &ba, 0, nullptr, sizeof bs, &bs);
    // float stereo at Barline's rate; the unit converts to whatever the device runs at
    AudioStreamBasicDescription f = {};
    f.mSampleRate = want_rate;
    f.mFormatID = kAudioFormatLinearPCM;
    f.mFormatFlags = kAudioFormatFlagIsFloat | kAudioFormatFlagIsPacked;
    f.mChannelsPerFrame = ch;
    f.mBitsPerChannel = 32;
    f.mBytesPerFrame = f.mBytesPerPacket = 4 * ch;
    f.mFramesPerPacket = 1;
    if (AudioUnitSetProperty(s->unit, kAudioUnitProperty_StreamFormat, kAudioUnitScope_Input, 0, &f, sizeof f) != noErr)
        return fail("it won't take Barline's sound format");
    AURenderCallbackStruct cb = {render, s};
    if (AudioUnitSetProperty(s->unit, kAudioUnitProperty_SetRenderCallback, kAudioUnitScope_Input, 0, &cb, sizeof cb) != noErr)
        return fail("couldn't play to it");
    // Barline's side of the buffer: the blocks asked for
    s->ring.init((size_t)want_period * std::max(2u, want_periods));
    if (AudioUnitInitialize(s->unit) != noErr) return fail("another program has it, or it went away");
    double dev_rate = 0;
    {
        AudioObjectPropertyAddress ra = {kAudioDevicePropertyNominalSampleRate, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        UInt32 rs = sizeof dev_rate;
        AudioObjectGetPropertyData(dev, &ra, 0, nullptr, &rs, &dev_rate);
    }
    UInt32 dev_frames = device_u32(dev, kAudioDevicePropertyLatency, kAudioObjectPropertyScopeOutput) +
                        device_u32(dev, kAudioDevicePropertySafetyOffset, kAudioObjectPropertyScopeOutput) +
                        device_u32(dev, kAudioDevicePropertyBufferFrameSize, kAudioObjectPropertyScopeGlobal);
    s->device_latency = dev_rate > 0 ? (long)(dev_frames * (double)want_rate / dev_rate) : (long)dev_frames;
    AudioObjectPropertyAddress alive = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
    AudioObjectAddPropertyListener(dev, &alive, on_alive, s);
    if (AudioOutputUnitStart(s->unit) != noErr) {
        AudioObjectRemovePropertyListener(dev, &alive, on_alive, s);
        AudioUnitUninitialize(s->unit);
        return fail("couldn't start it");
    }
    impl_ = s;
    fd_ = 0;
    rate = want_rate;
    channels = ch;
    in_ch_ = ch;
    period = want_period;
    periods = std::max(2u, want_periods);
    format = "float (Core Audio)";
    return true;
}

void Pcm::close() {
    if (auto *s = (Stream *)impl_) {
        AudioObjectPropertyAddress alive = {kAudioDevicePropertyDeviceIsAlive, kAudioObjectPropertyScopeGlobal, kAudioObjectPropertyElementMain};
        AudioObjectRemovePropertyListener(s->dev, &alive, on_alive, s);
        AudioOutputUnitStop(s->unit);
        AudioUnitUninitialize(s->unit);
        AudioComponentInstanceDispose(s->unit);
        delete s;
    }
    impl_ = nullptr;
    fd_ = -1;
}

bool Pcm::recover(int) { return false; }

// waits for room, like the Linux device does: the engine's timing comes from here
bool Pcm::write(const float *data, unsigned frames) {
    auto *s = (Stream *)impl_;
    if (!s) return false;
    unsigned done = 0;
    while (done < frames) {
        if (s->dead) return false;   // unplugged
        size_t room = s->ring.room();
        if (room == 0) {
            std::this_thread::sleep_for(std::chrono::microseconds(500));
            continue;
        }
        size_t n = std::min(room, (size_t)(frames - done));
        s->ring.push(data + (size_t)done * channels, n);
        done += (unsigned)n;
    }
    return true;
}

bool Pcm::read(float *, unsigned) { return false; }

long Pcm::delay() {
    auto *s = (Stream *)impl_;
    if (!s) return 0;
    return (long)s->ring.filled() + s->device_latency;
}

// ----------------------------------------------------------------------------- MIDI

bool Midi::open(int card, int device, bool in, bool out, std::string *err) {
    close();
    (void)card;
    MIDIEndpointRef dest;
    {
        std::lock_guard<std::mutex> lock(g_mu);
        if (device < 0 || device >= (int)g_midi_outs.size()) {
            if (err) *err = "that MIDI port isn't there";
            return false;
        }
        dest = g_midi_outs[(size_t)device];
    }
    MIDIClientRef c = midi_client();
    if (!c) {
        if (err) *err = "Core MIDI didn't answer";
        return false;
    }
    auto *m = new MidiPort();
    m->dest = dest;
    CFStringRef pn = CFStringCreateWithCString(nullptr, "Barline out", kCFStringEncodingUTF8);
    OSStatus st = out ? MIDIOutputPortCreate(c, pn, &m->out) : noErr;
    CFRelease(pn);
    if (st != noErr) {
        delete m;
        if (err) *err = "couldn't open a MIDI port";
        return false;
    }
    if (in) {   // the source with the same name as the destination
        CFStringRef want = nullptr;
        MIDIObjectGetStringProperty(dest, kMIDIPropertyDisplayName, &want);
        std::string wn = cf_string(want);
        if (want) CFRelease(want);
        for (ItemCount i = 0; i < MIDIGetNumberOfSources(); i++) {
            MIDIEndpointRef src = MIDIGetSource(i);
            CFStringRef n = nullptr;
            if (MIDIObjectGetStringProperty(src, kMIDIPropertyDisplayName, &n) != noErr || !n) continue;
            bool same = cf_string(n) == wn;
            CFRelease(n);
            if (!same) continue;
            CFStringRef inn = CFStringCreateWithCString(nullptr, "Barline in", kCFStringEncodingUTF8);
            if (MIDIInputPortCreate(c, inn, on_midi_in, m, &m->in) == noErr) {
                MIDIPortConnectSource(m->in, src, nullptr);
                m->src = src;
            }
            CFRelease(inn);
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
            if (m->src) MIDIPortDisconnectSource(m->in, m->src);
            MIDIPortDispose(m->in);
        }
        if (m->out) MIDIPortDispose(m->out);
        delete m;
    }
    impl_ = nullptr;
    fd_ = -1;
}

bool Midi::send(const uint8_t *b, size_t n) {
    auto *m = (MidiPort *)impl_;
    if (!m || !m->out) return false;
    Byte buf[512];
    auto *list = (MIDIPacketList *)buf;
    MIDIPacket *p = MIDIPacketListInit(list);
    // one message to a packet (a bank select and a program change can come together); time 0 = now
    size_t i = 0;
    while (i < n) {
        int len = common::message_length(b[i]);
        if (len == 0 || i + (size_t)len > n) {
            i++;
            continue;
        }
        p = MIDIPacketListAdd(list, sizeof buf, p, 0, (ByteCount)len, b + i);
        if (!p) return false;
        i += (size_t)len;
    }
    return MIDISend(m->out, m->dest, list) == noErr;   // (an error: the pedal went away)
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

#endif  // __APPLE__
