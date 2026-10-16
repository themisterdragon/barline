"""The kits Barline ships, where each comes from, and which of its keys plays which drum.

map: General MIDI note -> (SFZ program, its key[, {controller: value or "open"/"closed"}]).
"open"/"closed" picks whichever end of the hi-hat pedal controller rings longer or shorter.
raw: General MIDI note -> [file glob per velocity layer, soft to loud] (round-robins = the matches).
Licenses were checked in each source's own LICENSE file (2026-10-14).
"""


def gm(prog, notes, extra=None):
    """A kit already laid out on General MIDI notes: each note plays its own key."""
    out = {n: (prog, n) for n in notes}
    out.update(extra or {})
    return out


ROCK = [36, 37, 38, 40, 42, 44, 46, 43, 45, 47, 48, 49, 51, 53, 57]

KITS = {
    "virtuosity": {
        "name": "Virtuosity Studio",
        "by": "Versilian Studios and Karoryfer Samples",
        "license": "CC0-1.0",
        "url": "https://github.com/sfzinstruments/virtuosity_drums",
        "style": "studio",
        "note": "A polished, all-round studio kit with a full set of hand percussion.",
        "source": "virtuosity/src",
        "map": gm("Programs/02-full-kit.sfz", ROCK + [55, 52, 54, 56, 60, 61, 62, 63, 64, 69, 70, 73, 75, 76, 77]),
    },
    "naked": {
        "name": "Naked Drums",
        "by": "Wilkinson Audio",
        "license": "CC-BY-4.0",
        "url": "https://github.com/sfzinstruments/WilkinsonAudio.NakedDrums",
        "style": "rock",
        "note": "A natural, open-sounding pop/rock kit.",
        "source": "naked/src/Wilkinson Audio/Naked Drums",
        "map": gm("User/Naked Drums GM.sfz", ROCK + [52, 55]),
    },
    "bigrusty": {
        "name": "Big Rusty",
        "by": "Karoryfer Samples",
        "license": "CC0-1.0",
        "url": "https://github.com/sfzinstruments/karoryfer.big-rusty-drums",
        "style": "rock",
        "note": "A big, roomy 1980s kit.",
        "source": "bigrusty/src",
        "map": gm("Programs/01-full.sfz", [36, 37, 38, 40, 42, 44, 43, 45, 47, 49, 51, 53], {
            46: ("Programs/01-full.sfz", 46, {4: "open"}),
            48: ("Programs/01-full.sfz", 47),    # its smallest tom
            41: ("Programs/01-full.sfz", 41),    # the 22" floor tom
            57: ("Programs/01-full.sfz", 57),    # china, as the second crash
        }),
    },
    "gated80s": {
        "name": "Gated '80s",
        "by": "Karoryfer Samples (Big Rusty), gated reverb by Barline",
        "license": "CC0-1.0",
        "url": "https://github.com/sfzinstruments/karoryfer.big-rusty-drums",
        "style": "80s",
        "note": "Big Rusty with the 1980s treatment: a huge bright reverb on the snare and toms, chopped off by a gate.",
        "source": "bigrusty/src",
        # note -> how long the gate holds the reverb open (seconds)
        "gated": {38: 0.30, 40: 0.30, 37: 0.12, 39: 0.28, 43: 0.42, 45: 0.38, 47: 0.36, 48: 0.34, 36: 0.14},
        "map": gm("Programs/01-full.sfz", [36, 37, 38, 40, 42, 44, 43, 45, 47, 49, 51, 53], {
            46: ("Programs/01-full.sfz", 46, {4: "open"}),
            48: ("Programs/01-full.sfz", 47),
            57: ("Programs/01-full.sfz", 57),
        }),
    },
    "drs": {
        "name": "DRS Rock",
        "by": "DrumGizmo (Lars Muldjord, Bent Bisballe Nyeng)",
        "license": "CC-BY-4.0",
        "url": "https://github.com/sfzinstruments/DrumGizmo.DRSKit",
        "style": "rock",
        "note": "A hard-hitting rock kit.",
        "source": "drs/src/DrumGizmo/DRSKit",
        "map": gm("Stereo/DrumGizmo DRSKit.sfz", [36, 37, 38, 40, 42, 44, 49, 51, 53, 57], {
            46: ("Stereo/DrumGizmo DRSKit.sfz", 58),   # its open hi-hat key
            43: ("Stereo/DrumGizmo DRSKit.sfz", 41),   # Tom3 (floor)
            45: ("Stereo/DrumGizmo DRSKit.sfz", 43),   # Tom2
            48: ("Stereo/DrumGizmo DRSKit.sfz", 47),   # Tom1
        }),
    },
    "muldjord": {
        "name": "Muldjord",
        "by": "DrumGizmo (Lars Muldjord)",
        "license": "CC-BY-4.0",
        "url": "https://github.com/sfzinstruments/DrumGizmo.MuldjordKit",
        "style": "rock",
        "note": "A tight, punchy kit.",
        "source": "muldjord/src/DrumGizmo/MuldjordKit",
        "map": gm("Stereo/DrumGizmo MuldjordKit.sfz", [36, 37, 38, 42, 46, 45, 47, 48, 49, 51, 52, 53, 57], {
            43: ("Stereo/DrumGizmo MuldjordKit.sfz", 41),   # Tom4 (floor)
        }),
    },
    "swirly": {
        "name": "Swirly Brushes",
        "by": "Karoryfer Samples",
        "license": "CC0-1.0",
        "url": "https://github.com/sfzinstruments/karoryfer.swirly-drums",
        "style": "brushes",
        "note": "Played with brushes: soft, for ballads and acoustic sets. Note 39 is a brush sweep.",
        "source": "swirly/src",
        "names": {39: "Brush Sweep", 35: "Cajon Kick"},
        "map": gm("Programs/Full_kit.sfz", [35, 36, 38, 40, 42, 44, 43, 45, 47, 48, 49, 51, 55, 57, 56], {
            39: ("Programs/Full_kit.sfz", 60),   # snare stir
            46: ("Programs/Full_kit.sfz", 46, {4: "open"}),
            52: ("Programs/Full_kit.sfz", 53),   # china
            56: ("Programs/Full_kit.sfz", 66),   # cowbell
            60: ("Programs/Full_kit.sfz", 71),   # high bongo
            61: ("Programs/Full_kit.sfz", 69),   # low bongo
        }),
    },
    "gogodze": {
        "name": "Gogodze Phu",
        "by": "Karoryfer Samples",
        "license": "CC0-1.0",
        "url": "https://github.com/sfzinstruments/karoryfer.gogodze-phu-vol-ii",
        "style": "vintage",
        "note": "A small, warm 1970s-style kit; no cymbals of its own.",
        "source": "gogodze/src/Gogodze_Phu_vol_II",
        "map": {
            36: ("Programs/Kit.sfz", 36),
            38: ("Programs/Kit.sfz", 38),
            37: ("Programs/Kit.sfz", 37),
            40: ("Programs/Kit.sfz", 40),
            42: ("Programs/Kit.sfz", 46, {4: "closed"}),
            46: ("Programs/Kit.sfz", 46, {4: "open"}),
            44: ("Programs/Kit.sfz", 44),
            43: ("Programs/Kit.sfz", 41),
            45: ("Programs/Kit.sfz", 43),
            48: ("Programs/Kit.sfz", 45),
        },
    },
    "cajon": {
        "name": "Cajon & Shakers",
        "by": "Versilian Studios (VCSL)",
        "license": "CC0-1.0",
        "url": "https://github.com/sgossner/VCSL",
        "style": "acoustic",
        "note": "A cajon kit for unplugged sets: bass tone as kick, slap as snare, shakers as hi-hats.",
        "source": "vcsl/src",
        "rr": 4,
        "names": {36: "Cajon Bass", 38: "Cajon Slap", 37: "Cajon Tone", 42: "Shaker", 44: "Shaker Tap",
                  46: "Shaker Long"},
        "raw": {
            36: ["Idiophones/Struck Idiophones/Cajon/Cajon_hit3_p_rr*.wav", "Idiophones/Struck Idiophones/Cajon/Cajon_hit3_mp_rr*.wav",
                 "Idiophones/Struck Idiophones/Cajon/Cajon_hit3_f_rr*.wav"],
            38: ["Idiophones/Struck Idiophones/Cajon/Cajon_hit1_mp_rr*.wav", "Idiophones/Struck Idiophones/Cajon/Cajon_hit1_f_rr*.wav",
                 "Idiophones/Struck Idiophones/Cajon/Cajon_hit1_fff_rr*.wav"],
            37: ["Idiophones/Struck Idiophones/Cajon/Cajon_hit2_pp_rr*.wav", "Idiophones/Struck Idiophones/Cajon/Cajon_hit2_mp_rr*.wav",
                 "Idiophones/Struck Idiophones/Cajon/Cajon_hit2_f_rr*.wav"],
            42: ["Idiophones/Struck Idiophones/Shaker, Small/Mid_ShakerHighFaster_Down_rr*.wav"],
            44: ["Idiophones/Struck Idiophones/Shaker, Large/LShaker_Hit_rr*_Mid.wav"],
            46: ["Idiophones/Struck Idiophones/Shaker, Large/LShaker_Shake1*_Mid.wav"],
            70: ["Idiophones/Struck Idiophones/Shaker, Small/Mid_ShakerDouble_*_rr*.wav"],
            54: ["Idiophones/Struck Idiophones/Tambourine 1/Tamb1_Hit_v1_rr*_Mid.wav", "Idiophones/Struck Idiophones/Tambourine 1/Tamb1_Hit_v2_rr*_Mid.wav"],
            39: ["Idiophones/Struck Idiophones/Claps/Clap_rr*.wav"],
            56: ["Idiophones/Struck Idiophones/Cowbells/Cowbell1_Hit_v2_rr*_Mid.wav", "Idiophones/Struck Idiophones/Cowbells/Cowbell1_Hit_v3_rr*_Mid.wav",
                 "Idiophones/Struck Idiophones/Cowbells/Cowbell1_Hit_v4_rr*_Mid.wav"],
            75: ["Idiophones/Struck Idiophones/Claves/Claves1_Hit_v1_rr*_Mid.wav", "Idiophones/Struck Idiophones/Claves/Claves1_Hit_v2_rr*_Mid.wav",
                 "Idiophones/Struck Idiophones/Claves/Claves1_Hit_v3_rr*_Mid.wav"],
            76: ["Idiophones/Struck Idiophones/Woodblock/wood_click_f_rr*.wav"],
            69: ["Idiophones/Struck Idiophones/Cabasa/Cabasa1_Hit_rr*_Mid.wav"],
            73: ["Idiophones/Struck Idiophones/Guiro/Guiro_Hit_rr*_Mid.wav"],
            81: ["Idiophones/Struck Idiophones/Triangles/Triangle1_Hit_v1_rr*_Mid.wav"],
        },
    },
}
