// The looper pedal's MIDI remote control, exactly as its published user guide
// lists it: a note-on for each pedal function, and Program Change 0-127 to load a saved slot.
// The pedal only listens; it never says what it's doing.
#pragma once

namespace pedal {

enum Cmd {
    STOP = 7, STOP_B = 8, REC_PLAY = 11, REC_PLAY_B = 12, SPEED = 17, REVERSE = 29, FADE = 34, UNDO = 45, CLEAR = 49,
    NEXT_SLOT = 65, PREV_SLOT = 66, SOLO_A = 67, SOLO_B = 68, SOLO_CURRENT = 69, MUTE_A = 70, MUTE_B = 71, MUTE_CURRENT = 72,
    MODE_SINGLE = 73, MODE_MULTI = 74, MODE_SYNC = 75, MODE_SONG = 76, TRACK_FOCUS = 77, SELECT_A = 78, SELECT_B = 79
};

struct Info {
    Cmd cmd;
    const char *name;
};

inline const Info *all(int &count) {
    static const Info I[] = {{REC_PLAY, "Record / Play"},   {STOP, "Stop"},
                             {REC_PLAY_B, "Record / Play B"}, {STOP_B, "Stop B"},
                             {UNDO, "Undo / Redo"},          {CLEAR, "Clear"},
                             {FADE, "Fade"},                 {REVERSE, "Reverse"},
                             {SPEED, "Half Speed"},          {SELECT_A, "Select Track A"},
                             {SELECT_B, "Select Track B"},   {TRACK_FOCUS, "Switch Track"},
                             {SOLO_A, "Solo A"},             {SOLO_B, "Solo B"},
                             {SOLO_CURRENT, "Solo Current"}, {MUTE_A, "Mute A"},
                             {MUTE_B, "Mute B"},             {MUTE_CURRENT, "Mute Current"},
                             {NEXT_SLOT, "Load Next Slot"},  {PREV_SLOT, "Load Previous Slot"},
                             {MODE_SINGLE, "Mode: Single"},  {MODE_MULTI, "Mode: Multi"},
                             {MODE_SYNC, "Mode: Sync"},      {MODE_SONG, "Mode: Song"}};
    count = (int)(sizeof I / sizeof I[0]);
    return I;
}

inline const char *name(int cmd) {
    int n;
    const Info *I = all(n);
    for (int i = 0; i < n; i++)
        if (I[i].cmd == cmd) return I[i].name;
    return "?";
}

}  // namespace pedal
