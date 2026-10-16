// "barline doctor": checks the setup (the pedal, sound, MIDI, the iPad page, the window) and says
// what to do about anything that's off, in plain words. --fix makes the one change it can make itself.
#pragma once

namespace doctor {

int run(bool fix);

}  // namespace doctor
