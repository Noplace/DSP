#pragma once

// Editable song: the MIDI file as tick-based tracks of notes and other events. Playback event
// sequences are rebuilt from it after every edit (MidiSynth::PrepareSong / InstallSong).
//
// A file loads into this model without losing anything playback uses: every non-note event is
// kept with its position in the file ("order"), notes remember the order of their note-on and
// note-off and whether the file actually had the note-off. Rebuilding an unedited song gives the
// exact event stream the file had, so playback is bit-identical. Edited notes get orders that
// put their note-off first and their note-on last among events on the same tick.

#include <stdint.h>
#include <string>
#include <vector>

namespace dsp {
namespace audio {
namespace synth {

static const uint32_t kOrderFirst = 0;           // edited note-offs: before anything on their tick
static const uint32_t kOrderLast = 0xFFFFFFFFu;  // edited note-ons: after anything on their tick

struct SongNote {
  uint64_t start_tick = 0, end_tick = 0;
  float start_ms = 0, end_ms = 0;  // derived from the ticks by UpdateSongTiming
  uint8_t note = 60, velocity = 100, channel = 0, off_velocity = 64;
  bool off_event = true;           // a note-off event ends it (false: re-struck / never released in the file)
  bool selected = false;           // editor selection (not used by playback)
  uint32_t on_order = kOrderLast, off_order = kOrderFirst;
  // Call after changing a note's timing, pitch or channel.
  void MarkEdited() { off_event = true; on_order = kOrderLast; off_order = kOrderFirst; }
};

struct SongEvent {  // any non-note event, as the file had it
  uint64_t tick = 0;
  uint32_t order = 0;
  midi::Event ev;
};

struct SongTrack {
  std::string name;
  std::vector<SongNote> notes;    // sorted by start tick
  std::vector<SongEvent> events;  // sorted by (tick, order)
  bool created = false;           // made in the studio (shown even while empty)
  // derived
  uint16_t channel_mask = 0;
  int program = -1;               // first program change (-1 = none, 128 = drums)
  int low_note = 127, high_note = 0;
};

// Mixer automation: a parameter's value over song time. Linear between points; held before the
// first and after the last. strip -1 = master bus.
struct AutomationPoint {
  uint64_t tick = 0;
  float value = 0;
  double ms = 0;  // derived
};
struct AutomationLane {
  int strip = 0;
  int param = 0;
  std::vector<AutomationPoint> points;  // sorted by tick
};

struct SongBeat {
  double ms;
  int bar;   // 1-based
  int beat;  // 0 = downbeat
  uint64_t tick;
};

struct SongTempo {
  double ms;
  double bpm;
  uint64_t tick;
  uint32_t us_per_beat;
};

struct SongInfo {
  std::vector<SongTrack> tracks;  // index = file track index
  std::vector<AutomationLane> automation;
  uint32_t ticks_per_beat = 480;
  uint32_t next_order = 1;        // order for events added in the studio
  bool loaded = false;
  // derived by UpdateSongTiming
  std::vector<SongBeat> beats;    // runs at least 32 bars and 8 bars past the end
  std::vector<SongTempo> tempo;
  double length_ms = 0;           // last channel event
  uint64_t length_tick = 0;

  double TickToMs(uint64_t tick) const;
  uint64_t MsToTick(double ms) const;
  int TotalNotes() const;
};

// Builds the model from a parsed file.
void BuildSongFromFile(const midi::MidiLoader& file, SongInfo& song);
// An empty song (120 bpm, 4/4) to record or draw into.
void MakeEmptySong(SongInfo& song);
// Recomputes everything derived: tempo map, note times, beat grid, length, per-track summaries.
// Sorts notes by start and events by (tick, order).
void UpdateSongTiming(SongInfo& song);
// The track's playback events (notes as note-on/off plus its other events) in play order.
void BuildTrackEvents(const SongInfo& song, int track_index, std::vector<midi::Event>& out);
// Standard MIDI file, format 1. Text and SysEx payloads the loader does not keep are not written.
bool SaveSongAsMidi(const SongInfo& song, const char* path);

}
}
}
