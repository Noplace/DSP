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
    for (auto& row : cc_map_) for (auto& m : row) m.store(-1, std::memory_order_relaxed);
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
  // The song being played. Changed only through LoadMidi / InstallSong (with the audio thread
  // kept out), so the thread doing those may read it freely.
  const SongInfo& song() const { return song_; }

  // Playback event sequence of one track.
  struct Track {
    std::vector<midi::Event> events;
    midi::Event* event_sequence = nullptr;
    uint32_t event_index = 0, event_count = 0, ticks_to_next_event = 0;
    void Bind() {
      event_sequence = events.empty() ? nullptr : events.data();
      event_count = uint32_t(events.size());
    }
  };
  // A song with its playback events built, ready to swap in. PrepareSong touches no synth
  // state, so it runs on any thread; InstallSong is quick and must run with the audio thread
  // kept out. position_ms < 0 starts the song from scratch (as loading a file does);
  // otherwise playback carries on from that position (sounding notes are released).
  struct PreparedSong {
    SongInfo song;
    std::vector<Track> tracks;
  };
  static PreparedSong PrepareSong(SongInfo song);
  void InstallSong(PreparedSong&& prepared, double position_ms);

  // Transport (any thread).
  double PlaybackMs() const override { return position_ms_.load(std::memory_order_relaxed); }
  // Tempo factor (0.25-2): the song plays faster or slower, pitch unchanged.
  void set_speed(double speed) { speed_ = speed < 0.25 ? 0.25 : speed > 2.0 ? 2.0 : speed; }
  double speed() const { return speed_.load(); }
  // Loop between two song positions (ms) while on.
  void set_loop(bool on, double start_ms, double end_ms) {
    loop_start_ms_ = start_ms;
    loop_end_ms_ = end_ms;
    loop_on_ = on;
  }
  bool loop_on() const { return loop_on_.load(); }
  double loop_start_ms() const { return loop_start_ms_.load(); }
  double loop_end_ms() const { return loop_end_ms_.load(); }
  // Click on every beat of the song's grid while playing.
  void set_metronome(bool on) { metronome_on_ = on; }
  bool metronome() const { return metronome_on_.load(); }
  void set_metronome_level(float level) { metronome_level_ = level; }
  // Clicks for total_ms (one every beat_ms) before the sequencer moves; starts on the next block.
  void StartCountIn(double total_ms, double beat_ms) {
    count_in_beat_ms_ = beat_ms;
    count_in_request_ms_ = total_ms;
  }
  bool counting_in() const { return count_in_frames_.load() > 0 || count_in_request_ms_.load() > 0; }
  // Keep the transport running past the last event (while recording).
  void set_hold_at_end(bool hold) { hold_at_end_ = hold; }

  // Notes currently held on each channel (key down; any thread). Bit n of the result = channel n.
  uint16_t channels_holding(int note) const {
    uint16_t mask = 0;
    for (int ch = 0; ch < 16; ++ch)
      if ((held_[ch][(note >> 6) & 1].load(std::memory_order_relaxed) >> (note & 63)) & 1) mask |= uint16_t(1 << ch);
    return mask;
  }

  // Automation: the parameter being dragged in the UI is left alone (strip -1 = master;
  // param -1 = none).
  void set_touched_param(int strip, int param) {
    touched_param_ = param < 0 ? -1 : (strip + 1) * 1024 + param;
  }
  // Automation value of a lane at a song position.
  static float AutomationValue(const AutomationLane& lane, double ms);

  // MIDI learn: the next controller (CC) from live input is bound to the parameter; mapped CCs
  // then drive their parameter (over its whole range) and no longer reach the channel.
  void StartMidiLearn(int strip, int param) { learn_target_ = (strip + 1) * 1024 + param; }
  void CancelMidiLearn() { learn_target_ = -1; }
  bool midi_learning(int strip, int param) const { return learn_target_.load() == (strip + 1) * 1024 + param; }
  int TakeLearned() { return learned_.exchange(-1); }  // (channel << 8) | cc, or -1
  // (channel << 8) | cc bound to the parameter, or -1.
  int MidiMappingFor(int strip, int param) const;
  void SetMidiMapping(int channel, int cc, int strip, int param);  // param -1 = unbind the CC
  void ClearMidiMapping(int strip, int param);
  bool cc_mapped(int channel, int cc) const { return cc_map_[channel & 15][cc & 127].load() >= 0; }
  // target as strip/param for a mapped CC (false if none)
  bool MappedTarget(int channel, int cc, int* strip, int* param) const;
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
  static const MidiEventHandler midi_main_event_handlers[5];
  static const MidiEventHandler midi_meta_event_handlers[23];
  static const MidiEventHandler midi_channel_event_handlers[23];
  void DestroyTrackData() {
    track_store_.clear();
    track_count_ = 0;
    tracks = nullptr;
  }
  void BindTracks() {
    for (auto& t : track_store_) t.Bind();
    tracks = track_store_.empty() ? nullptr : track_store_.data();
    track_count_ = int(track_store_.size());
  }
  void ResetTracks() {
    for (int track_index=0;track_index<track_count_;++track_index) {
      auto& track = tracks[track_index];
      track.event_index = 0;
      track.ticks_to_next_event = track.event_count ? track.event_sequence[0].deltaTime : 0xFFFFFFFF;
    }
    position_ms_ = 0;
    last_event = GetNextEvent();
    samples_to_next_event = 0;
  }
  void BuildInstrumentBank();
  void RenderSequencer(uint32_t samples_count, real_t* data_out);
  void MixClicks(real_t* data_out, uint32_t frames, double start_ms, double ms_per_frame);
  void StartClick(bool downbeat);
  struct Click {
    float phase = 0, inc = 0, amp = 0, decay = 0;
  } click_;
  uint32_t count_in_beat_frames_ = 0, count_in_done_ = 0;
  std::atomic<double> position_ms_{0}, speed_{1.0}, loop_start_ms_{0}, loop_end_ms_{0};
  std::atomic<bool> loop_on_{false}, metronome_on_{false}, hold_at_end_{false};
  std::atomic<float> metronome_level_{0.35f};
  std::atomic<double> count_in_request_ms_{0}, count_in_beat_ms_{500};
  std::atomic<uint32_t> count_in_frames_{0};
  std::vector<Track> track_store_;
  void ApplyAutomation(double ms);
  std::atomic<int> touched_param_{-1}, learn_target_{-1}, learned_{-1};
  std::atomic<int> cc_map_[16][128];  // encoded target or -1
  std::atomic<uint64_t> held_[16][2] = {};  // notes down per channel (for the UI keyboard)
  void SetHeld(int ch, int note, bool on) {
    auto& word = held_[ch & 15][(note >> 6) & 1];
    const uint64_t bit = uint64_t(1) << (note & 63);
    if (on) word.fetch_or(bit, std::memory_order_relaxed);
    else word.fetch_and(~bit, std::memory_order_relaxed);
  }
  void ResetChannelsToDefaults();
  void ApplyChannelProgram(int channel);  // file program + overrides -> channel instrument
  void ApplyOverridesIfChanged();
  void ChaseSetup(double from_ms);
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