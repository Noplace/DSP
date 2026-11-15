/*****************************************************************************************************************
* Copyright (c) 2012 Khalid Ali Al-Kooheji                                                                       *
*                                                                                                                *
* Permission is hereby granted, free of charge, to any person obtaining a copy of this software and              *
* associated documentation files (the "Software"), to deal in the Software without restriction, including        *
* without limitation the rights to use, copy, modify, merge, publish, distribute, sublicense, and/or sell        *
* copies of the Software, and to permit persons to whom the Software is furnished to do so, subject to the       *
* following conditions:                                                                                          *
*                                                                                                                *
* The above copyright notice and this permission notice shall be included in all copies or substantial           *
* portions of the Software.                                                                                      *
*                                                                                                                *
* THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT NOT          *
* LIMITED TO THE WARRANTIES OF MERCHANTABILITY, * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT.          *
* IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, * DAMAGES OR OTHER LIABILITY,      *
* WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT OF OR IN CONNECTION WITH THE            *
* SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.                                                         *
*****************************************************************************************************************/
#pragma once



namespace dsp {
namespace audio {
namespace synth {

template<typename F>
static void for_each_note(F f) {
  real_t A4_freq = 440;
  real_t two = pow(2.0f,1/12.0f);
  for (int i=0;i<128;++i) {
    auto freq = A4_freq * pow(two,i-69);//69 = index of A4
    f(i,freq);
  }
}

// What the UI shows of a loaded file: tracks with their notes, bar/beat grid and tempo map.
// Built by LoadMidi(); only the thread that loads files touches it.
struct SongNote {
  float start_ms, end_ms;
  uint8_t note, velocity, channel;
};
struct SongTrack {
  std::string name;
  std::vector<SongNote> notes;   // sorted by start time
  uint16_t channel_mask = 0;     // channels the track's channel events use
  int program = -1;              // first program change (-1 = none)
  int low_note = 127, high_note = 0;
};
struct SongBeat {
  double ms;
  int bar;   // 1-based
  int beat;  // 0 = downbeat
};
struct SongTempo {
  double ms;
  double bpm;
};
struct SongInfo {
  std::vector<SongTrack> tracks;  // index = file track index
  std::vector<SongBeat> beats;
  std::vector<SongTempo> tempo;
  double length_ms = 0;
  bool loaded = false;
};

class MidiSynth : public Synth {
 public:
  typedef void (MidiSynth::*MidiEventHandler)(midi::Event* event);
  enum State {
    kStateStopped=0,kStatePlaying=1,kStatePaused=2
  };
  enum Mode {
    kModeSequencer,
    kModeTest
  };
  MidiSynth() : Synth(),initialized_(false),mode_(kModeSequencer),drum_kit_(nullptr),
                last_event(nullptr),tracks(nullptr),ticks_per_beat(0),samples_to_next_event(0),bpm(0),track_count_(0) {
    memset(&instr,0,sizeof(instr));
    memset(&channels,0,sizeof(channels));
    memset(&aux_effects,0,sizeof(aux_effects));
    memset(&main_effects,0,sizeof(main_effects));
  }
  ~MidiSynth() {
    Deinitialize();
  }
  void Initialize();
  void Deinitialize();
  void LoadMidiFromFile(const char* filename);
  void LoadMidi(uint8_t* data, size_t data_size);
  void RenderSamplesStereo(uint32_t samples_count, real_t* data_out);
  void RenderIdleStereo(uint32_t samples_count, real_t* data_out) override;
  void HandleMidiMessage(uint8_t status, uint8_t data1, uint8_t data2) override;
  void AllNotesOff() override;
  void Reset() {
    ResetTracks();
  }
  midi::Event* BeginPrepareNextEvent() {
    auto result = GetNextEvent();
    EnterCriticalSection(&me_lock);
    if (tracks[0].event_index)
      --tracks[0].event_index;
    return result;
  }
  void EndPrepareNextEvent() {
    LeaveCriticalSection(&me_lock);
  }
  void set_mode(Mode mode) {
    mode_ = mode;
  }
  // Folder with calibration files (*.cal) for instruments modelled on recordings. If not set,
  // Initialize() looks for "data\calibration" next to or above the executable.
  void set_calibration_dir(const std::string& dir) { calibration_dir_ = dir; }
  const std::string& calibration_dir() const { return calibration_dir_; }
  // Play every melodic channel with one GM program (0-127) regardless of the file's program
  // changes; -1 = follow the file.
  void set_program_override(int program);
  int program_override() const { return program_override_; }
  // Names of the calibrated instruments in use (empty if none were found).
  const std::vector<std::string>& calibrated_instruments() const { return calibrated_instruments_; }

  void Seek(double ms) override;
  const SongInfo& song() const { return song_; }
  // Track mute/solo (file tracks 0..63): muted tracks start no new notes. Any thread.
  void set_track_muted(int track, bool muted) { SetTrackBit(track_mute_mask_, track, muted); }
  void set_track_solo(int track, bool solo) { SetTrackBit(track_solo_mask_, track, solo); }
  bool track_muted(int track) const { return track < 64 && (track_mute_mask_.load() >> track) & 1; }
  bool track_solo(int track) const { return track < 64 && (track_solo_mask_.load() >> track) & 1; }
  // GM program currently selected on a channel (any thread; approximate while it changes).
  // 128 = drum kit.
  int channel_program(int channel) const { return channel_program_[channel & 15].load(std::memory_order_relaxed); }
  // Play a channel with a fixed instrument (0-127 GM program, 128 = drum kit) whatever the file
  // selects; -1 = follow the file. Any thread: applied by the audio thread on its next block.
  void set_channel_override(int channel, int program) {
    channel_override_[channel & 15].store(program, std::memory_order_relaxed);
    overrides_dirty_.store(true, std::memory_order_release);
  }
  int channel_override(int channel) const { return channel_override_[channel & 15].load(std::memory_order_relaxed); }

  // Channel strips, FX buses and master bus between the channels and the output.
  effects::Mixer mixer;
  effects::Effect* aux_effects[kEffectCount];
  effects::Effect* main_effects[kEffectCount];
  effects::Delay delay_unit;
 private:
  struct Track {
    midi::Event* event_sequence;
    uint32_t event_index,event_count,ticks_to_next_event;
    __forceinline midi::Event* GetCurrentEvent() {
      return &event_sequence[event_index];
    }
  };
  static const MidiEventHandler midi_main_event_handlers[5];
  static const MidiEventHandler midi_meta_event_handlers[23];
  static const MidiEventHandler midi_channel_event_handlers[23];
  void DestroyTrackData() {
    for (int i=0;i<track_count_;++i)
      SafeDeleteArray(&tracks[i].event_sequence);
    SafeDeleteArray(&tracks);
    track_count_ = 0;
    tracks = nullptr;
  }
  void ResetTracks() {
    for (int track_index=0;track_index<track_count_;++track_index) {
      auto& track = tracks[track_index];
      track.event_index = 0;      
      track.ticks_to_next_event = track.event_sequence[0].deltaTime;
    }
    last_event = GetNextEvent();
    samples_to_next_event = 0;
  }
  void BuildInstrumentBank();
  void BuildSongInfo(const midi::MidiLoader& file);
  void ResetChannelsToDefaults();
  void ApplyChannelProgram(int channel);  // file program + overrides -> channel instrument
  void ApplyOverridesIfChanged();
  static void SetTrackBit(std::atomic<uint64_t>& mask, int track, bool on) {
    if (track < 0 || track >= 64) return;
    if (on) mask.fetch_or(uint64_t(1) << track); else mask.fetch_and(~(uint64_t(1) << track));
  }
  bool TrackAudible(uint8_t track) const {
    if (track >= 64) return true;  // live input
    const uint64_t bit = uint64_t(1) << track;
    const uint64_t solo = track_solo_mask_.load(std::memory_order_relaxed);
    if (track_mute_mask_.load(std::memory_order_relaxed) & bit) return false;
    return solo == 0 || (solo & bit) != 0;
  }
  SongInfo song_;
  std::atomic<uint64_t> track_mute_mask_{0}, track_solo_mask_{0};
  std::atomic<int> channel_program_[16] = {};
  int file_program_[16] = {};  // last program change from the file/live input (128 = drums)
  std::atomic<int> channel_override_[16] = {-1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1, -1};
  std::atomic<bool> overrides_dirty_{false};
  void GenerateIntoBufferStereo(uint32_t samples_to_generate, real_t* data_out, uint32_t& data_offset);
  void GenerateChunkStereo(uint32_t samples_to_generate, real_t* data_out, uint32_t& data_offset);
  void MixChannelsStereo(uint32_t samples_to_generate);
  midi::Event* GetNextEvent();
  void HandleEvent(midi::Event* event);
  void MidiEventUnknown(midi::Event* event);
  void MidiEventMeta(midi::Event* event);
  void MidiEventSetTempo(midi::Event* event);
  void MidiEventChannel(midi::Event* event);
  void MidiEventNoteOn(midi::Event* event);
  void MidiEventNoteOff(midi::Event* event);
  void MidiEventProgramChange(midi::Event* event);
  void MidiEventController(midi::Event* event);
  void MidiEventPitchBend(midi::Event* event);
  real_t chromatic_scale_freq[note_count];
  struct {
    real_t* channels[16];
    real_t* mono[16];
    real_t* main;
    real_t* aux;
    size_t main_size;
    size_t aux_size;
  } buffers;  
  instruments::InstrumentProcessor* instr[kInstrumentCount];  // GM program -> instrument (not owned)
  std::vector<std::unique_ptr<instruments::InstrumentProcessor>> instrument_store_;
  std::string calibration_dir_;
  std::vector<std::string> calibrated_instruments_;
  int program_override_ = -1;
  instruments::InstrumentProcessor* drum_kit_;  // channel 10 (owned by instrument_store_)
  Channel* channels[kChannelCount];
  midi::Event* last_event;
  Track* tracks;
  CRITICAL_SECTION me_lock;
  double bpm;
  uint32_t ticks_per_beat;
  uint32_t samples_to_next_event;
  int track_count_;
  Mode mode_;
  bool initialized_;
};

}
}
}