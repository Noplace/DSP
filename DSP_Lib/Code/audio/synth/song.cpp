#define NOMINMAX  // std::min/std::max below
#include <algorithm>
#include <stdio.h>
#include "../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {

// ---------------------------------------------------------------------------------------------
// Tempo map. The arithmetic matches what the loader always did (same operations in the same
// order), so event times come out bit-identical.
// ---------------------------------------------------------------------------------------------
double SongInfo::TickToMs(uint64_t tick) const {
  const double tpb = ticks_per_beat ? double(ticks_per_beat) : 480.0;
  if (tempo.empty()) return double(tick) * 500000u / (1000.0 * tpb);
  // last tempo point at or before tick
  size_t lo = 0, hi = tempo.size();
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (tempo[mid].tick <= tick) lo = mid; else hi = mid;
  }
  const auto& p = tempo[lo];
  return p.ms + double(tick - p.tick) * p.us_per_beat / (1000.0 * tpb);
}

uint64_t SongInfo::MsToTick(double ms) const {
  const double tpb = ticks_per_beat ? double(ticks_per_beat) : 480.0;
  if (ms <= 0) return 0;
  if (tempo.empty()) return uint64_t(ms * 1000.0 * tpb / 500000.0 + 0.5);
  size_t i = 0;
  while (i + 1 < tempo.size() && tempo[i + 1].ms <= ms) ++i;
  const auto& p = tempo[i];
  return p.tick + uint64_t((ms - p.ms) * 1000.0 * tpb / p.us_per_beat + 0.5);
}

int SongInfo::TotalNotes() const {
  int n = 0;
  for (auto& t : tracks) n += int(t.notes.size());
  return n;
}

// ---------------------------------------------------------------------------------------------
// Loading
// ---------------------------------------------------------------------------------------------
void BuildSongFromFile(const midi::MidiLoader& file, SongInfo& song) {
  song = SongInfo();
  song.ticks_per_beat = file.ticksPerBeat;
  uint32_t order = 1;
  const int track_count = int(std::min<uint32_t>(file.track_count, 64));
  song.tracks.resize(track_count);
  for (int t = 0; t < track_count; ++t) {
    SongTrack& track = song.tracks[t];
    track.name = file.track_names[t];
    int open[16][128];
    for (auto& row : open) for (auto& v : row) v = -1;
    uint64_t tick = 0;
    for (const auto& source : file.eventList[t]) {
      tick += source.deltaTime;
      midi::Event ev = source;
      if (ev.type == midi::kEventTypeChannel && ev.subtype == midi::kEventSubtypeNoteOff && ev.data.note.velocity == 0)
        ev.data.note.velocity = 127;  // as playback always treated it
      const int ch = ev.channel & 15;
      if (ev.type == midi::kEventTypeChannel && ev.subtype == midi::kEventSubtypeNoteOn) {
        const int key = ev.data.note.noteNumber & 127;
        if (open[ch][key] >= 0) {  // re-struck without a note-off
          auto& prev = track.notes[open[ch][key]];
          prev.end_tick = tick;
          prev.off_event = false;
        }
        SongNote n;
        n.start_tick = n.end_tick = tick;
        n.note = uint8_t(key);
        n.velocity = ev.data.note.velocity;
        n.channel = uint8_t(ch);
        n.on_order = order;
        n.off_event = false;
        open[ch][key] = int(track.notes.size());
        track.notes.push_back(n);
      } else if (ev.type == midi::kEventTypeChannel && ev.subtype == midi::kEventSubtypeNoteOff &&
                 open[ch][ev.data.note.noteNumber & 127] >= 0) {
        auto& n = track.notes[open[ch][ev.data.note.noteNumber & 127]];
        n.end_tick = tick;
        n.off_event = true;
        n.off_velocity = ev.data.note.velocity;
        n.off_order = order;
        open[ch][ev.data.note.noteNumber & 127] = -1;
      } else {
        SongEvent se;
        se.tick = tick;
        se.order = order;
        se.ev = ev;
        track.events.push_back(se);
      }
      ++order;
    }
    for (int ch = 0; ch < 16; ++ch)
      for (int key = 0; key < 128; ++key)
        if (open[ch][key] >= 0) track.notes[open[ch][key]].end_tick = tick;  // never released
  }
  song.next_order = order;
  song.loaded = true;
  UpdateSongTiming(song);
}

void MakeEmptySong(SongInfo& song) {
  song = SongInfo();
  song.ticks_per_beat = 480;
  SongTrack conductor;
  conductor.name = "Conductor";
  SongEvent tempo;
  tempo.order = song.next_order++;
  tempo.ev.type = midi::kEventTypeMeta;
  tempo.ev.subtype = midi::kEventSubtypeSetTempo;
  tempo.ev.data.tempo.microsecondsPerBeat = 500000;
  conductor.events.push_back(tempo);
  SongEvent sig;
  sig.order = song.next_order++;
  sig.ev.type = midi::kEventTypeMeta;
  sig.ev.subtype = midi::kEventSubtypeTimeSignature;
  sig.ev.data.time_signature.numerator = 4;
  sig.ev.data.time_signature.denominator = 4;
  sig.ev.data.time_signature.metronome = 24;
  sig.ev.data.time_signature.thirtyseconds = 8;
  conductor.events.push_back(sig);
  song.tracks.push_back(conductor);
  song.loaded = true;
  UpdateSongTiming(song);
}

// ---------------------------------------------------------------------------------------------
// Derived data
// ---------------------------------------------------------------------------------------------
void UpdateSongTiming(SongInfo& song) {
  for (auto& t : song.tracks) {
    std::stable_sort(t.notes.begin(), t.notes.end(),
                     [](const SongNote& a, const SongNote& b) { return a.start_tick < b.start_tick; });
    std::stable_sort(t.events.begin(), t.events.end(), [](const SongEvent& a, const SongEvent& b) {
      return a.tick != b.tick ? a.tick < b.tick : a.order < b.order;
    });
  }

  // Tempo map: tempo changes from every track, in track order then file order, stable-sorted by
  // tick (the order the loader used).
  struct Change { uint64_t tick; uint32_t us; };
  std::vector<Change> changes;
  for (auto& t : song.tracks)
    for (auto& e : t.events)
      if (e.ev.type == midi::kEventTypeMeta && e.ev.subtype == midi::kEventSubtypeSetTempo &&
          e.ev.data.tempo.microsecondsPerBeat != 0)
        changes.push_back({e.tick, e.ev.data.tempo.microsecondsPerBeat});
  std::stable_sort(changes.begin(), changes.end(), [](const Change& a, const Change& b) { return a.tick < b.tick; });
  const double tpb = song.ticks_per_beat ? double(song.ticks_per_beat) : 480.0;
  song.tempo.clear();
  song.tempo.push_back({0.0, 120.0, 0, 500000u});  // until the first tempo event
  double ms = 0;
  uint64_t last_tick = 0;
  uint32_t us_per_beat = 500000;
  for (auto& c : changes) {
    ms += double(c.tick - last_tick) * us_per_beat / (1000.0 * tpb);
    last_tick = c.tick;
    us_per_beat = c.us;
    song.tempo.push_back({ms, 60000000.0 / c.us, c.tick, c.us});
  }

  for (auto& lane : song.automation) {
    std::stable_sort(lane.points.begin(), lane.points.end(),
                     [](const AutomationPoint& a, const AutomationPoint& b) { return a.tick < b.tick; });
    for (auto& p : lane.points) p.ms = song.TickToMs(p.tick);
  }

  // Note times, length and per-track summaries.
  uint64_t max_tick = 0;
  bool any_channel = false;
  for (auto& t : song.tracks) {
    t.channel_mask = 0;
    t.program = -1;
    t.low_note = 127;
    t.high_note = 0;
    for (auto& n : t.notes) {
      n.start_ms = float(song.TickToMs(n.start_tick));
      n.end_ms = float(song.TickToMs(n.end_tick));
      t.channel_mask |= uint16_t(1 << (n.channel & 15));
      t.low_note = std::min<int>(t.low_note, n.note);
      t.high_note = std::max<int>(t.high_note, n.note);
      max_tick = std::max(max_tick, n.off_event ? n.end_tick : n.start_tick);
      any_channel = true;
    }
    for (auto& e : t.events) {
      if (e.ev.type != midi::kEventTypeChannel) continue;
      t.channel_mask |= uint16_t(1 << (e.ev.channel & 15));
      max_tick = std::max(max_tick, e.tick);
      any_channel = true;
      if (t.program < 0 && e.ev.subtype == midi::kEventSubtypeProgramChange)
        t.program = (e.ev.channel & 15) == 9 ? 128 : e.ev.data.program.number;
    }
    if (t.program < 0 && (t.channel_mask & (1 << 9))) t.program = 128;
  }
  song.length_tick = any_channel ? max_tick : 0;
  song.length_ms = any_channel ? song.TickToMs(max_tick) : 0.0;

  // Bar/beat grid from the time signatures, to well past the end (for editing and recording).
  struct Signature { uint64_t tick; int num, den; };
  std::vector<Signature> sigs;
  for (auto& t : song.tracks)
    for (auto& e : t.events)
      if (e.ev.type == midi::kEventTypeMeta && e.ev.subtype == midi::kEventSubtypeTimeSignature &&
          e.ev.data.time_signature.numerator > 0 && e.ev.data.time_signature.denominator > 0)
        sigs.push_back({e.tick, e.ev.data.time_signature.numerator, e.ev.data.time_signature.denominator});
  std::stable_sort(sigs.begin(), sigs.end(), [](const Signature& a, const Signature& b) { return a.tick < b.tick; });
  song.beats.clear();
  const uint64_t bar_ticks = uint64_t(tpb) * 4;
  const uint64_t end_tick = std::max(song.length_tick + bar_ticks * 8, bar_ticks * 32);
  int num = 4, den = 4, bar = 1, beat = 0;
  size_t next_sig = 0;
  uint64_t tick = 0;
  while (tick <= end_tick && song.beats.size() < 200000) {
    while (next_sig < sigs.size() && sigs[next_sig].tick <= tick) {
      num = sigs[next_sig].num;
      den = sigs[next_sig].den;
      if (beat != 0) { ++bar; beat = 0; }  // a new signature starts a new bar
      ++next_sig;
    }
    song.beats.push_back({song.TickToMs(tick), bar, beat, tick});
    const uint64_t step = uint64_t(tpb * 4 / den);
    tick += step ? step : 1;
    if (++beat >= num) { beat = 0; ++bar; }
  }
}

// ---------------------------------------------------------------------------------------------
// Playback events
// ---------------------------------------------------------------------------------------------
void BuildTrackEvents(const SongInfo& song, int track_index, std::vector<midi::Event>& out) {
  const SongTrack& track = song.tracks[track_index];
  struct Item {
    uint64_t tick;
    uint32_t order;
    uint32_t seq;
    midi::Event ev;
  };
  std::vector<Item> items;
  items.reserve(track.notes.size() * 2 + track.events.size());
  uint32_t seq = 0;
  for (const auto& n : track.notes) {
    Item on;
    on.tick = n.start_tick;
    on.order = n.on_order;
    on.seq = seq++;
    on.ev.type = midi::kEventTypeChannel;
    on.ev.subtype = midi::kEventSubtypeNoteOn;
    on.ev.channel = n.channel;
    on.ev.data.note.noteNumber = n.note;
    on.ev.data.note.velocity = n.velocity;
    items.push_back(on);
    if (n.off_event) {
      Item off;
      off.tick = n.end_tick;
      off.order = n.off_order;
      off.seq = seq++;
      off.ev.type = midi::kEventTypeChannel;
      off.ev.subtype = midi::kEventSubtypeNoteOff;
      off.ev.channel = n.channel;
      off.ev.data.note.noteNumber = n.note;
      off.ev.data.note.velocity = n.off_velocity;
      items.push_back(off);
    }
  }
  for (const auto& e : track.events) {
    Item it;
    it.tick = e.tick;
    it.order = e.order;
    it.seq = seq++;
    it.ev = e.ev;
    items.push_back(it);
  }
  std::sort(items.begin(), items.end(), [](const Item& a, const Item& b) {
    if (a.tick != b.tick) return a.tick < b.tick;
    if (a.order != b.order) return a.order < b.order;
    return a.seq < b.seq;
  });
  out.resize(items.size());
  uint64_t prev = 0;
  for (size_t i = 0; i < items.size(); ++i) {
    midi::Event& ev = out[i];
    ev = items[i].ev;
    ev.deltaTime = uint32_t(items[i].tick - prev);
    ev.abs_time_ms = song.TickToMs(items[i].tick);
    ev.track = uint8_t(track_index);
    prev = items[i].tick;
  }
}

// ---------------------------------------------------------------------------------------------
// Writing
// ---------------------------------------------------------------------------------------------
namespace {
void PutVarInt(std::vector<uint8_t>& out, uint32_t v) {
  uint8_t bytes[5];
  int n = 0;
  bytes[n++] = uint8_t(v & 0x7F);
  while (v >>= 7) bytes[n++] = uint8_t((v & 0x7F) | 0x80);
  while (n) out.push_back(bytes[--n]);
}
void Put32(std::vector<uint8_t>& out, uint32_t v) {
  for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s));
}
void Put16(std::vector<uint8_t>& out, uint32_t v) {
  out.push_back(uint8_t(v >> 8));
  out.push_back(uint8_t(v));
}
}  // namespace

bool SaveSongAsMidi(const SongInfo& song, const char* path) {
  std::vector<uint8_t> file;
  file.insert(file.end(), {'M', 'T', 'h', 'd'});
  Put32(file, 6);
  Put16(file, 1);
  Put16(file, uint32_t(song.tracks.size()));
  Put16(file, song.ticks_per_beat ? song.ticks_per_beat : 480);
  std::vector<midi::Event> events;
  for (int t = 0; t < int(song.tracks.size()); ++t) {
    std::vector<uint8_t> data;
    const std::string& name = song.tracks[t].name;
    if (!name.empty()) {
      data.push_back(0);
      data.insert(data.end(), {0xFF, 0x03});
      PutVarInt(data, uint32_t(name.size()));
      data.insert(data.end(), name.begin(), name.end());
    }
    BuildTrackEvents(song, t, events);
    uint64_t tick = 0, written = 0;
    for (const auto& ev : events) {
      tick += ev.deltaTime;
      std::vector<uint8_t> body;
      if (ev.type == midi::kEventTypeChannel) {
        const uint8_t ch = ev.channel & 15;
        switch (ev.subtype) {
          case midi::kEventSubtypeNoteOn: body = {uint8_t(0x90 | ch), ev.data.note.noteNumber, ev.data.note.velocity}; break;
          case midi::kEventSubtypeNoteOff: body = {uint8_t(0x80 | ch), ev.data.note.noteNumber, uint8_t(ev.data.note.velocity & 0x7F)}; break;
          case midi::kEventSubtypeController: body = {uint8_t(0xB0 | ch), ev.data.controller.type, ev.data.controller.value}; break;
          case midi::kEventSubtypeProgramChange: body = {uint8_t(0xC0 | ch), ev.data.program.number}; break;
          case midi::kEventSubtypePitchBend:
            body = {uint8_t(0xE0 | ch), uint8_t(ev.data.pitch.value & 0x7F), uint8_t((ev.data.pitch.value >> 7) & 0x7F)};
            break;
          case midi::kEventSubtypeNoteAftertouch:
            body = {uint8_t(0xA0 | ch), ev.data.note_aftertouch.noteNumber, ev.data.note_aftertouch.amount};
            break;
          case midi::kEventSubtypeChannelAftertouch: body = {uint8_t(0xD0 | ch), ev.data.channel_aftertouch.amount}; break;
          default: break;
        }
      } else if (ev.type == midi::kEventTypeMeta) {
        switch (ev.subtype) {
          case midi::kEventSubtypeSetTempo: {
            const uint32_t us = ev.data.tempo.microsecondsPerBeat;
            body = {0xFF, 0x51, 0x03, uint8_t(us >> 16), uint8_t(us >> 8), uint8_t(us)};
            break;
          }
          case midi::kEventSubtypeTimeSignature: {
            uint8_t log2_den = 0;
            while ((1u << log2_den) < ev.data.time_signature.denominator && log2_den < 7) ++log2_den;
            body = {0xFF, 0x58, 0x04, ev.data.time_signature.numerator, log2_den, ev.data.time_signature.metronome,
                    ev.data.time_signature.thirtyseconds};
            break;
          }
          case midi::kEventSubtypeKeySignature:
            body = {0xFF, 0x59, 0x02, ev.data.key_signature.key, ev.data.key_signature.scale};
            break;
          default: break;  // end of track is written below; text payloads are not kept by the loader
        }
      }
      if (body.empty()) continue;
      PutVarInt(data, uint32_t(tick - written));
      written = tick;
      data.insert(data.end(), body.begin(), body.end());
    }
    PutVarInt(data, uint32_t(tick - written));  // end of track at the last event (including a trailing EOT delta)
    data.insert(data.end(), {0xFF, 0x2F, 0x00});
    file.insert(file.end(), {'M', 'T', 'r', 'k'});
    Put32(file, uint32_t(data.size()));
    file.insert(file.end(), data.begin(), data.end());
  }
  FILE* f = nullptr;
  if (fopen_s(&f, path, "wb") != 0 || !f) return false;
  const bool ok = fwrite(file.data(), 1, file.size(), f) == file.size();
  return fclose(f) == 0 && ok;
}

}
}
}
