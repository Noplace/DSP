#pragma once

namespace dsp {
namespace audio {
namespace synth {

// The drum kit's mixer groups. On channel 10 every kit piece plays into one of these groups;
// each group has its own mixer strip (EQ, gate, compressor, sends...) and the groups add up into
// channel 10's strip, which works as the drum bus. A piece always plays into one group, so its
// strokes stay together (hi-hat open/closed/pedal, snare hit/rim/side stick).
enum DrumGroup {
  kDrumKick, kDrumSnare, kDrumToms, kDrumHiHat, kDrumCymbals, kDrumRide, kDrumPercussion, kDrumOther,
  kDrumGroupCount
};

inline const char* DrumGroupName(int group) {
  static const char* names[kDrumGroupCount] = {"Kick", "Snare", "Toms", "Hi-Hat", "Cymbals", "Ride", "Percussion", "FX"};
  return group >= 0 && group < kDrumGroupCount ? names[group] : "";
}

// Group of a GM drum note (27-87).
inline int DrumGroupOfNote(int note) {
  switch (note) {
    case 35: case 36: return kDrumKick;
    case 37: case 38: case 40: return kDrumSnare;
    case 41: case 43: case 45: case 47: case 48: case 50: return kDrumToms;
    case 42: case 44: case 46: return kDrumHiHat;
    case 49: case 52: case 55: case 57: return kDrumCymbals;
    case 51: case 53: case 59: return kDrumRide;
    case 27: case 28: case 29: case 30: case 31: case 32: case 33: case 34: case 71: case 72: return kDrumOther;
    default: return kDrumPercussion;  // clap, hand percussion, triangle, shakers, surdo...
  }
}

}
}
}
