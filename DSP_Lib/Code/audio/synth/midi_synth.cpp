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
#include <algorithm>
#include "../../dsp.h"


//#define OUTPUT_EVENTS
namespace dsp {
namespace audio {
namespace synth {

const char* eventnames[] = {"Meta",
"SysEx",
"DividedSysEx",
"Channel",
"Unknown"
};
const char* subeventnames[] = {
"Text",
"CopyrightNotice",
"TrackName",
"InstrumentName",
"Lyrics",
"Marker",
"CuePoint",
"MidiChannelPrefix",
"EndOfTrack",
"SetTempo",
"SmpteOffset",
"TimeSignature",
"KeySignature",
"SequencerSpecific",
"SequenceNumber",
"NoteOff",
"NoteOn",
"NoteAftertouch",
"Controller",
"ProgramChange",
"ChannelAftertouch",
"PitchBend",
"Unknown",
};


const MidiSynth::MidiEventHandler MidiSynth::midi_main_event_handlers[5] = {
  &MidiSynth::MidiEventMeta,    &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, 
  &MidiSynth::MidiEventChannel, &MidiSynth::MidiEventUnknown, 
};

const MidiSynth::MidiEventHandler MidiSynth::midi_meta_event_handlers[23] = {
  &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown,
  &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventSetTempo, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown,
  &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown,  &MidiSynth::MidiEventUnknown,
};

const MidiSynth::MidiEventHandler MidiSynth::midi_channel_event_handlers[23] = {
  &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown,
  &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventNoteOff, 
  &MidiSynth::MidiEventNoteOn, &MidiSynth::MidiEventUnknown, &MidiSynth::MidiEventController, &MidiSynth::MidiEventProgramChange,  &MidiSynth::MidiEventUnknown,&MidiSynth::MidiEventPitchBend, &MidiSynth::MidiEventUnknown,

};

// General MIDI program -> instrument. Programs that should sound alike share one instrument
// (and its voice pool). Plucked and struck strings use digital waveguides; everything else uses
// oscillator waves with an envelope that suits the family.
void MidiSynth::BuildInstrumentBank() {
  using instruments::Envelope;
  using instruments::OscWave;

  auto add = [&](instruments::InstrumentProcessor* inst) {
    inst->set_sample_rate(sample_rate_);
    inst->Prepare();  // load up front: live program changes happen on the audio thread
    instrument_store_.emplace_back(inst);
    return inst;
  };
  auto assign = [&](int first, int last, instruments::InstrumentProcessor* inst) {
    for (int p = first; p <= last; ++p)
      instr[p] = inst;
  };

  //                         attack sustain  attack  decay  release (levels, ms)
  const Envelope pluck   = {0.9f,  0.0f,     2.0f,  700.0f, 150.0f};
  const Envelope organ   = {0.45f, 0.45f,    6.0f,    1.0f,  60.0f};
  const Envelope strings = {0.35f, 0.32f,  140.0f,   60.0f, 300.0f};
  const Envelope brass   = {0.45f, 0.35f,   45.0f,   90.0f, 120.0f};
  const Envelope reed    = {0.35f, 0.30f,   30.0f,   60.0f, 100.0f};
  const Envelope flute   = {0.5f,  0.42f,   70.0f,   60.0f, 140.0f};
  const Envelope lead    = {0.35f, 0.30f,    5.0f,   50.0f,  80.0f};
  const Envelope pad     = {0.3f,  0.28f,  450.0f,  100.0f, 700.0f};
  const Envelope fx      = {0.4f,  0.30f,  200.0f,  200.0f, 500.0f};

  using instruments::WaveguideString;
  // Calibrated from recordings of a real instrument when the calibration file is available,
  // otherwise the hand-set waveguide preset.
  calibrated_instruments_.clear();
  instruments::InstrumentProcessor* harpsichord = nullptr;
  if (!calibration_dir_.empty()) {
    auto cal = std::make_shared<instruments::StringCalibration>();
    std::string path = calibration_dir_ + "\\harpsichord_english.cal";
    if (cal->Load(path.c_str()) && !cal->notes.empty()) {
      instruments::CalibratedStringOptions opt;
      opt.release_t60_s = 0.12f;
      opt.level = 0.35f;
      harpsichord = add(new instruments::CalibratedString(cal, opt));
      calibrated_instruments_.push_back("harpsichord (" + cal->name + ")");
    }
  }
  if (!harpsichord)
    harpsichord = add(new WaveguideString(WaveguideString::Harpsichord()));

  // Grand piano from recordings (with velocity layers); honky-tonk = the same piano with its
  // unison strings far out of tune.
  instruments::InstrumentProcessor* grand = nullptr;
  instruments::InstrumentProcessor* honky_tonk = nullptr;
  if (!calibration_dir_.empty()) {
    auto cal = std::make_shared<instruments::StringCalibration>();
    std::string path = calibration_dir_ + "\\piano_steinway_b.cal";
    if (cal->Load(path.c_str()) && !cal->notes.empty()) {
      instruments::CalibratedStringOptions opt;
      opt.release_t60_s = 0.3f;
      opt.level = 0.35f;
      opt.detune_cents = 0.6f;        // aftersound vs first stage (slow beating)
      opt.recorded_tuning = true;     // the piano's own stretched tuning
      opt.undamped_from_note = 89;    // top strings have no dampers (F6 up)
      opt.modal_max_hz = 4000.0f;     // low partials with their measured two-stage decay
      grand = add(new instruments::CalibratedString(cal, opt));
      opt.unison_detune_cents = 12.0f;
      honky_tonk = add(new instruments::CalibratedString(cal, opt));
      calibrated_instruments_.push_back("piano (" + cal->name + ")");
    }
  }
  auto piano       = add(new WaveguideString(WaveguideString::Piano()));
  auto guitar      = add(new WaveguideString(WaveguideString::NylonGuitar()));
  auto steel       = add(new WaveguideString(WaveguideString::SteelGuitar()));
  auto muted       = add(new WaveguideString(WaveguideString::MutedGuitar()));
  auto bass        = add(new WaveguideString(WaveguideString::Bass()));
  auto harp        = add(new WaveguideString(WaveguideString::Harp()));
  auto tri_organ   = add(new OscWave(OscWave::Triangle, organ));
  auto sq_organ    = add(new OscWave(OscWave::Square, Envelope{0.25f, 0.25f, 6.0f, 1.0f, 60.0f}));
  auto saw_strings = add(new OscWave(OscWave::Sawtooth, strings));
  auto saw_brass   = add(new OscWave(OscWave::Sawtooth, brass));
  auto sq_reed     = add(new OscWave(OscWave::Square, reed));
  auto tri_flute   = add(new OscWave(OscWave::Triangle, flute));
  auto sq_lead     = add(new OscWave(OscWave::Square, lead));
  auto saw_lead    = add(new OscWave(OscWave::Sawtooth, lead));
  auto saw_pluck   = add(new OscWave(OscWave::Sawtooth, Envelope{0.5f, 0.0f, 2.0f, 500.0f, 120.0f}));
  auto tri_pad     = add(new OscWave(OscWave::Triangle, pad));
  auto saw_pad     = add(new OscWave(OscWave::Sawtooth, Envelope{0.2f, 0.18f, 450.0f, 100.0f, 700.0f}));
  auto tri_fx      = add(new OscWave(OscWave::Triangle, fx));

  using instruments::ModalInstrument;
  auto modal = [&](const instruments::ModalParams& p) { return add(new ModalInstrument(p)); };
  auto kit = new instruments::DrumKit();  // physically modelled kit (drum_kit.h), fitted to recordings if found
  if (!calibration_dir_.empty() && kit->LoadCalibration((calibration_dir_ + "\\drums_vcsl.cal").c_str()))
    calibrated_instruments_.push_back("drum kit (" + kit->calibration_name() + ")");
  drum_kit_ = add(kit);

  assign(0, 5, piano);           // pianos (preset) ...
  if (grand) {                   // ... or calibrated from recordings
    assign(0, 2, grand);         // acoustic, bright, electric grand
    assign(3, 3, honky_tonk);
  }
  assign(6, 7, harpsichord);     // harpsichord, clavinet
  assign(8, 8, modal(ModalInstrument::Celesta()));
  assign(9, 9, modal(ModalInstrument::Glockenspiel()));
  assign(10, 10, modal(ModalInstrument::MusicBox()));
  assign(11, 11, modal(ModalInstrument::Vibraphone()));
  assign(12, 12, modal(ModalInstrument::Marimba()));
  assign(13, 13, modal(ModalInstrument::Xylophone()));
  assign(14, 14, modal(ModalInstrument::TubularBells()));
  assign(15, 15, harp);          // dulcimer
  assign(16, 18, tri_organ);     // drawbar / percussive / rock organ
  assign(19, 20, sq_organ);      // church / reed organ
  assign(21, 23, sq_reed);       // accordion, harmonica, bandoneon
  assign(24, 24, guitar);        // nylon guitar
  assign(25, 27, steel);         // steel, jazz, clean electric
  assign(28, 28, muted);         // muted guitar
  assign(29, 30, saw_lead);      // overdriven / distortion guitar
  assign(31, 31, steel);         // guitar harmonics
  assign(32, 37, bass);          // acoustic / electric / slap bass
  assign(38, 39, saw_pluck);     // synth bass
  assign(40, 44, saw_strings);   // violin .. tremolo strings
  assign(45, 45, muted);         // pizzicato strings
  assign(46, 46, harp);          // orchestral harp
  assign(47, 47, modal(ModalInstrument::Timpani()));
  assign(48, 51, saw_strings);   // string ensembles
  assign(52, 54, tri_pad);       // choir, voice oohs, synth voice
  assign(55, 55, saw_pluck);     // orchestra hit
  assign(56, 63, saw_brass);     // brass
  assign(64, 71, sq_reed);       // saxes, oboe, english horn, bassoon, clarinet
  assign(72, 79, tri_flute);     // piccolo .. ocarina
  assign(80, 80, sq_lead);       // square lead
  assign(81, 81, saw_lead);      // saw lead
  assign(82, 83, tri_flute);     // calliope, chiff
  assign(84, 84, saw_lead);      // charang
  assign(85, 85, tri_pad);       // voice lead
  assign(86, 87, saw_lead);      // fifths, bass + lead
  assign(88, 89, tri_pad);       // new age, warm
  assign(90, 90, saw_pad);       // polysynth
  assign(91, 91, tri_pad);       // choir pad
  assign(92, 93, saw_pad);       // bowed, metallic
  assign(94, 94, tri_pad);       // halo
  assign(95, 95, saw_pad);       // sweep
  assign(96, 103, tri_fx);       // synth effects
  assign(104, 104, harpsichord); // sitar
  assign(105, 105, steel);       // banjo
  assign(106, 107, harpsichord); // shamisen, koto
  assign(108, 108, modal(ModalInstrument::Kalimba()));
  assign(109, 109, sq_reed);     // bagpipe
  assign(110, 110, saw_strings); // fiddle
  assign(111, 111, sq_reed);     // shanai
  auto bells = modal(ModalInstrument::TubularBells());
  assign(112, 112, bells);       // tinkle bell
  assign(113, 113, modal(ModalInstrument::Agogo()));
  assign(114, 114, modal(ModalInstrument::SteelDrums()));
  assign(115, 115, modal(ModalInstrument::Woodblock()));
  assign(116, 117, modal(ModalInstrument::Taiko()));   // taiko, melodic tom
  assign(118, 118, modal(ModalInstrument::SynthDrum()));
  assign(119, 127, tri_fx);      // reverse cymbal, sound effects
}

// "data\calibration" in the executable's folder or one of its parents (build output lives a few
// levels below the repository root).
static std::string FindCalibrationDir() {
  char path[MAX_PATH];
  DWORD len = GetModuleFileNameA(nullptr, path, MAX_PATH);
  if (len == 0 || len >= MAX_PATH) return std::string();
  std::string dir(path, len);
  for (int level = 0; level < 6; ++level) {
    size_t slash = dir.find_last_of("\\/");
    if (slash == std::string::npos) break;
    dir.resize(slash);
    std::string candidate = dir + "\\data\\calibration";
    DWORD attr = GetFileAttributesA(candidate.c_str());
    if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) return candidate;
  }
  return std::string();
}

void MidiSynth::Initialize() {
  if (initialized_ == true)
    return;
  if (calibration_dir_.empty())
    calibration_dir_ = FindCalibrationDir();

  {
    static const real_t A4_freq = 440;
    for (int i=0;i<note_count;++i) {
      chromatic_scale_freq[i] = A4_freq * pow(_2_POW_12TH,i-69);//45 = index of A4
    }
  }


  auto buffer_length_ms_ = 400.0f;//400ms
  auto buffer_size = uint32_t(sample_rate_*2*buffer_length_ms_*0.001);
  buffers.aux_size = buffers.main_size = buffer_size;
  buffers.main = new real_t[buffers.main_size]; //44100 samples & channels , check with player later on!
  buffers.aux = new real_t[buffers.aux_size]; //44100 samples & channels , check with player later on!

  last_event = nullptr;
  samples_to_next_event = 0;

  BuildInstrumentBank();

  for (int i=0;i<kChannelCount;++i) {
    buffers.channels[i] = new real_t[buffers.main_size];
    memset(buffers.channels[i],0,sizeof(real_t)*buffers.main_size);
    buffers.mono[i] = new real_t[buffers.main_size / 2];
    channels[i] = new Channel(i);
    channels[i]->set_sample_rate(sample_rate_);
    channels[i]->set_buffer(buffers.channels[i]);
    channels[i]->set_mono_buffer(buffers.mono[i], uint32_t(buffers.main_size / 2));
    channels[i]->set_instrument(instr[0]);
    channels[i]->set_silence(true);
    channels[i]->set_panning(0.5f);
    channels[i]->set_amplification(100 / 127.0f);  // GM default volume (CC7 = 100)
  }
  channels[9]->set_instrument(drum_kit_);  // GM channel 10: drum kit

  delay_unit.Initialize(uint32_t(sample_rate_*2));
  delay_unit.set_feedback(0.3f);
  delay_unit.set_delay_ms(400.0f);

  mixer.Initialize(sample_rate_, uint32_t(buffers.main_size / 2));
  for (int i = 0; i < kChannelCount; ++i) {
    file_program_[i] = channel_program_[i] = i == 9 ? 128 : 0;  // 128 = drum kit
    mixer.set_midi_send(i, effects::Mixer::kBusReverb, 40 / 127.0f);  // GM default CC91
    mixer.set_midi_send(i, effects::Mixer::kBusChorus, 0.0f);
  }

  tracks = nullptr;

  if (mode_ == kModeTest) {
    track_store_.resize(1);
    track_store_[0].events.resize(20);
    BindTracks();
    bpm = 120; //qn per min
    channels[0]->set_silence(false);
  }
  InitializeCriticalSection(&me_lock);
  initialized_ = true;
}

void MidiSynth::Deinitialize() {
  if (initialized_ == false)
    return;
  DeleteCriticalSection(&me_lock);
  delay_unit.Deinitialize();
  mixer.Deinitialize();
  DestroyTrackData();
  for (int i=0;i<kChannelCount;++i) {
    SafeDelete(&channels[i]);
    SafeDeleteArray(&buffers.channels[i]);
    SafeDeleteArray(&buffers.mono[i]);
  }
  // instr[] and drum_kit_ only point into the store (several programs share one instrument).
  memset(instr, 0, sizeof(instr));
  drum_kit_ = nullptr;
  instrument_store_.clear();
  SafeDeleteArray(&buffers.aux);
  SafeDeleteArray(&buffers.main);
  initialized_ = false;
}

void MidiSynth::LoadMidiFromFile(const char* filename) {
  uint8_t* buffer;
  size_t size=0;
  io::ReadWholeFileBinary(filename,&buffer,size);
  if (size != 0) {
    LoadMidi(buffer,size);
    io::DestroyFileBuffer(&buffer);
  }
}

void MidiSynth::LoadMidi(uint8_t* data, size_t data_size) {
  if (mode_ == kModeTest)
    return;
  midi::MidiLoader midifile;
  midifile.Load(data,data_size);
  SongInfo song;
  BuildSongFromFile(midifile, song);
  InstallSong(PrepareSong(std::move(song)), -1.0);
}

MidiSynth::PreparedSong MidiSynth::PrepareSong(SongInfo song) {
  PreparedSong prepared;
  UpdateSongTiming(song);
  prepared.tracks.resize(song.tracks.size());
  for (size_t t = 0; t < song.tracks.size(); ++t)
    BuildTrackEvents(song, int(t), prepared.tracks[t].events);
  prepared.song = std::move(song);
  return prepared;
}

void MidiSynth::InstallSong(PreparedSong&& prepared, double position_ms) {
  if (mode_ == kModeTest)
    return;
  const bool fresh = position_ms < 0;
  if (fresh) {
    player_->Stop();
    // Every file starts from GM defaults, not from where the previous file left the channels.
    AllNotesOff();
    ResetChannelsToDefaults();
  }
  song_ = std::move(prepared.song);
  track_store_ = std::move(prepared.tracks);
  BindTracks();
  ticks_per_beat = song_.ticks_per_beat;
  bpm = 120;  // until the song's first tempo event
  player_->song_length_ms = song_.length_ms;
  for (auto& track : track_store_) {
    for (auto& ev : track.events) {
      if (ev.type != midi::kEventTypeChannel) continue;
      channels[ev.channel & 15]->set_silence(false);
      if (ev.subtype == midi::kEventSubtypeProgramChange) {
        if ((ev.channel & 15) == 9) drum_kit_->Load();
        else instr[ev.data.program.number & 127]->Load();
      }
    }
  }
  if (fresh) {
    track_mute_mask_ = 0;
    track_solo_mask_ = 0;
    if (program_override_ >= 0)
      set_program_override(program_override_);  // songs without program changes start on it too
    Reset();
    ChaseSetup(0.0);  // instruments in place before playback starts
  } else {
    Seek(position_ms);  // same place in the edited song
  }
}


// Instrument for a channel: its per-channel override if set, else the drum kit on channel 10,
// else the global program override, else the program the file/live input selected.
void MidiSynth::ApplyChannelProgram(int i) {
  int program = channel_override_[i].load(std::memory_order_relaxed);
  if (program < 0) {
    if (i == 9) program = 128;
    else if (program_override_ >= 0) program = program_override_ & 127;
    else program = file_program_[i] & 127;
  }
  channels[i]->set_instrument(program >= 128 ? drum_kit_ : instr[program & 127]);
  channel_program_[i] = program >= 128 ? 128 : program;
}

void MidiSynth::ApplyOverridesIfChanged() {
  if (!initialized_ || !overrides_dirty_.exchange(false, std::memory_order_acquire))
    return;
  for (int i = 0; i < kChannelCount; ++i)
    ApplyChannelProgram(i);
}

void MidiSynth::ResetChannelsToDefaults() {
  for (int i = 0; i < kChannelCount; ++i) {
    auto c = channels[i];
    file_program_[i] = i == 9 ? 128 : 0;
    ApplyChannelProgram(i);
    mixer.set_midi_send(i, effects::Mixer::kBusReverb, 40 / 127.0f);  // GM defaults: CC91 = 40,
    mixer.set_midi_send(i, effects::Mixer::kBusChorus, 0.0f);         // CC93 = 0
    c->ResetControllers();
    c->set_amplification(100 / 127.0f);
    c->set_panning(0.5f);
    c->set_pitch_bend_range(2.0f);
    c->param_coarse = c->param_fine = 127;
    c->des_coarse = 2;
    c->des_fine = 0;
  }
}

// Rewinds every track, then replays everything but notes up to the target so programs,
// controllers and tempo are what they would be there, and schedules the first event after it.
void MidiSynth::Seek(double target_ms) {
  if (target_ms < 0) target_ms = 0;
  position_ms_ = target_ms;
  if (!initialized_ || mode_ != kModeSequencer || track_count_ == 0 || tracks == nullptr)
    return;
  AllNotesOff();
  ResetChannelsToDefaults();
  for (int t = 0; t < track_count_; ++t) {
    tracks[t].event_index = 0;
    tracks[t].ticks_to_next_event = tracks[t].event_count ? tracks[t].event_sequence[0].deltaTime : 0xFFFFFFFF;
  }
  bpm = 120;
  last_event = nullptr;
  samples_to_next_event = 0xFFFFFFFF;
  if (target_ms < 0) target_ms = 0;
  while (true) {
    samples_to_next_event = 0;
    midi::Event* ev = GetNextEvent();  // at the end this stops the player, which is right
    if (ev == nullptr) {
      samples_to_next_event = 0xFFFFFFFF;
      break;
    }
    const bool is_note = ev->subtype == midi::kEventSubtypeNoteOn || ev->subtype == midi::kEventSubtypeNoteOff;
    // Stop at the first event after the target, or at the first note exactly on it.
    if (ev->abs_time_ms > target_ms || (is_note && ev->abs_time_ms >= target_ms)) {
      last_event = ev;
      samples_to_next_event = uint32_t((ev->abs_time_ms - target_ms) * 0.001 * sample_rate_ / speed_);
      break;
    }
    if (ev->subtype != midi::kEventSubtypeNoteOn && ev->subtype != midi::kEventSubtypeNoteOff)
      HandleEvent(ev);
  }
  ChaseSetup(target_ms);
}

// Chase: a channel whose part hasn't started by from_ms gets the set-up events (program, volume,
// pan...) it has before its first note, so a song positioned before a part starts already has
// that part's instrument. They fire again at their own time during playback, so this changes
// nothing audible (no voices sound on the channel before its first note).
void MidiSynth::ChaseSetup(double from_ms) {
  double first_note[16];
  for (auto& f : first_note) f = 1e300;
  for (int t = 0; t < track_count_; ++t)
    for (uint32_t i = 0; i < tracks[t].event_count; ++i) {
      const auto& ev = tracks[t].event_sequence[i];
      if (ev.type == midi::kEventTypeChannel && ev.subtype == midi::kEventSubtypeNoteOn &&
          ev.abs_time_ms < first_note[ev.channel & 15])
        first_note[ev.channel & 15] = ev.abs_time_ms;
    }
  for (int t = 0; t < track_count_; ++t) {
    bool note_in_track[16] = {};
    for (uint32_t i = 0; i < tracks[t].event_count; ++i) {
      midi::Event* ev = &tracks[t].event_sequence[i];
      if (ev->type != midi::kEventTypeChannel) continue;
      const int ch = ev->channel & 15;
      if (ev->subtype == midi::kEventSubtypeNoteOn || ev->subtype == midi::kEventSubtypeNoteOff) {
        note_in_track[ch] = true;
        continue;
      }
      if (first_note[ch] >= from_ms && !note_in_track[ch] && ev->abs_time_ms <= first_note[ch])
        HandleEvent(ev);
    }
  }
}


void MidiSynth::RenderIdleStereo(uint32_t samples_count, real_t* data_out) {
  ApplyOverridesIfChanged();
  uint32_t data_offset = 0;
  GenerateIntoBufferStereo(samples_count, data_out, data_offset);
}

void MidiSynth::AllNotesOff() {
  for (int i=0;i<kChannelCount;++i) {
    if (channels[i] != nullptr) {
      channels[i]->set_sustain(false);  // a pedal left down by the file would hold new notes
      channels[i]->AllNotesOff();
      held_[i][0] = 0;
      held_[i][1] = 0;
    }
  }
}

void MidiSynth::HandleMidiMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  if (!initialized_ || status < 0x80 || status >= 0xF0)
    return;  // running status is resolved by the driver; ignore system messages
  const uint8_t type = status & 0xF0;
  const uint8_t channel = status & 0x0F;
  data1 &= 0x7F;
  data2 &= 0x7F;

  if (type == 0xB0) {  // MIDI learn and mapped controllers
    const int learn = learn_target_.load(std::memory_order_relaxed);
    if (learn >= 0) {
      for (auto& row : cc_map_)  // one CC per parameter
        for (auto& m : row)
          if (m.load(std::memory_order_relaxed) == learn) m = -1;
      cc_map_[channel][data1] = learn;
      learn_target_ = -1;
      learned_ = (channel << 8) | data1;
      return;
    }
    int strip, param;
    if (MappedTarget(channel, data1, &strip, &param)) {
      const auto& info = strip < 0 ? effects::MasterParamInfo(param) : effects::StripParamInfo(param);
      const float v = effects::ParamFromNormalized(info, data2 / 127.0f);
      if (strip < 0) mixer.master().set(param, v);
      else mixer.strip(strip).set(param, v);
      return;
    }
  }

  midi::Event ev;
  ev.type = midi::kEventTypeChannel;
  ev.channel = channel;
  ev.track = 0xFF;  // live input: not subject to file track mute/solo
  switch (type) {
    case 0x80:
      ev.subtype = midi::kEventSubtypeNoteOff;
      ev.data.note.noteNumber = data1;
      ev.data.note.velocity = data2 ? data2 : 64;
      break;
    case 0x90:
      ev.data.note.noteNumber = data1;
      if (data2 == 0) {  // note-on with velocity 0 means note-off
        ev.subtype = midi::kEventSubtypeNoteOff;
        ev.data.note.velocity = 64;
      } else {
        ev.subtype = midi::kEventSubtypeNoteOn;
        ev.data.note.velocity = data2;
      }
      break;
    case 0xB0:
      ev.subtype = midi::kEventSubtypeController;
      ev.data.controller.type = data1;
      ev.data.controller.value = data2;
      break;
    case 0xC0:
      ev.subtype = midi::kEventSubtypeProgramChange;
      ev.data.program.number = data1;
      break;
    case 0xE0:
      ev.subtype = midi::kEventSubtypePitchBend;
      ev.data.pitch.value = uint16_t(data1 | (data2 << 7));
      break;
    default:
      return;  // aftertouch etc. not handled yet
  }
  channels[channel]->set_silence(false);
  HandleEvent(&ev);
}

// Renders the next block: count-in clicks first if one was requested, then the sequencer, split
// at the loop end (playback jumps back to the loop start, sample-accurately).
void MidiSynth::RenderSamplesStereo(uint32_t samples_count, real_t* data_out) {
  ApplyOverridesIfChanged();
  uint32_t data_offset = 0;
  if (mode_ == kModeTest) {
    samples_to_next_event = samples_count;
    auto samples_to_generate = samples_to_next_event;
    if (samples_to_generate > 0) {
      GenerateIntoBufferStereo(samples_to_generate,data_out,data_offset);
      samples_count -= samples_to_generate;
      samples_to_next_event = 0xFFFFFFFF;
    }
    if (last_event != nullptr) {
      HandleEvent(last_event);
      memset(last_event,0,sizeof(midi::Event));
    }
    last_event = GetNextEvent();
    return;
  }

  uint32_t done = 0;
  if (!song_.automation.empty()) ApplyAutomation(position_ms_);
  const double count_in_ms = count_in_request_ms_.exchange(0);
  if (count_in_ms > 0) {
    count_in_frames_ = uint32_t(count_in_ms * 0.001 * sample_rate_);
    count_in_beat_frames_ = std::max<uint32_t>(1, uint32_t(count_in_beat_ms_ * 0.001 * sample_rate_));
    count_in_done_ = 0;
  }
  if (count_in_frames_ > 0) {  // the song waits; live input still plays
    const uint32_t n = std::min<uint32_t>(samples_count, count_in_frames_);
    GenerateIntoBufferStereo(n, data_out, data_offset);
    for (uint32_t i = 0; i < n; ++i, ++count_in_done_) {
      if (count_in_done_ % count_in_beat_frames_ == 0) StartClick(count_in_done_ == 0);
      if (click_.amp > 1e-4f) {
        const float s = sinf(click_.phase) * click_.amp;
        click_.phase += click_.inc;
        click_.amp *= click_.decay;
        data_out[2 * i] += s;
        data_out[2 * i + 1] += s;
      }
    }
    count_in_frames_ -= n;
    done = n;
  }

  while (done < samples_count) {
    uint32_t segment = samples_count - done;
    bool wrap = false;
    const double loop_start = loop_start_ms_, loop_end = loop_end_ms_;
    if (loop_on_ && loop_end - loop_start >= 20.0) {
      double pos = position_ms_;
      if (pos >= loop_end) {  // switched on (or moved) behind the playhead
        Seek(loop_start);
        pos = loop_start;
      }
      const double frames_left = (loop_end - pos) * 0.001 * sample_rate_ / speed_;
      if (frames_left < segment) {
        segment = std::max<uint32_t>(1, uint32_t(ceil(frames_left)));
        wrap = true;
      }
    }
    RenderSequencer(segment, data_out + done * 2);
    done += segment;
    if (wrap) {
      Seek(loop_start);
      if (!song_.automation.empty()) ApplyAutomation(position_ms_);  // values at the loop start
    }
  }
}

float MidiSynth::AutomationValue(const AutomationLane& lane, double ms) {
  const auto& pts = lane.points;
  if (pts.empty()) return 0;
  if (ms <= pts.front().ms) return pts.front().value;
  if (ms >= pts.back().ms) return pts.back().value;
  size_t lo = 0, hi = pts.size() - 1;
  while (hi - lo > 1) {
    const size_t mid = (lo + hi) / 2;
    if (pts[mid].ms <= ms) lo = mid; else hi = mid;
  }
  const auto& a = pts[lo];
  const auto& b = pts[hi];
  const auto& info = lane.strip < 0 ? effects::MasterParamInfo(lane.param) : effects::StripParamInfo(lane.param);
  using U = effects::ParamUnit;
  if (info.unit == U::kSwitch || info.unit == U::kChannel || info.unit == U::kChoice) return a.value;  // steps
  const double t = b.ms > a.ms ? (ms - a.ms) / (b.ms - a.ms) : 0.0;
  return float(a.value + (b.value - a.value) * t);
}

// Writes every automated parameter's value at ms into the mixer (the mixer smooths the steps).
void MidiSynth::ApplyAutomation(double ms) {
  const int touched = touched_param_.load(std::memory_order_relaxed);
  for (const auto& lane : song_.automation) {
    if (lane.points.empty() || lane.param < 0) continue;
    if (touched == (lane.strip + 1) * 1024 + lane.param) continue;  // the user has hold of it
    const float v = AutomationValue(lane, ms);
    if (lane.strip < 0) {
      if (lane.param < effects::kMasterParamCount) mixer.master().set(lane.param, v);
    } else if (lane.strip < 16 && lane.param < effects::kStripParamCount) {
      mixer.strip(lane.strip).set(lane.param, v);
    }
  }
}

int MidiSynth::MidiMappingFor(int strip, int param) const {
  const int target = (strip + 1) * 1024 + param;
  for (int ch = 0; ch < 16; ++ch)
    for (int cc = 0; cc < 128; ++cc)
      if (cc_map_[ch][cc].load(std::memory_order_relaxed) == target) return (ch << 8) | cc;
  return -1;
}

void MidiSynth::SetMidiMapping(int channel, int cc, int strip, int param) {
  cc_map_[channel & 15][cc & 127] = param < 0 ? -1 : (strip + 1) * 1024 + param;
}

void MidiSynth::ClearMidiMapping(int strip, int param) {
  const int target = (strip + 1) * 1024 + param;
  for (auto& row : cc_map_)
    for (auto& m : row)
      if (m.load(std::memory_order_relaxed) == target) m = -1;
}

bool MidiSynth::MappedTarget(int channel, int cc, int* strip, int* param) const {
  const int target = cc_map_[channel & 15][cc & 127].load(std::memory_order_relaxed);
  if (target < 0) return false;
  *strip = target / 1024 - 1;
  *param = target % 1024;
  return true;
}

void MidiSynth::RenderSequencer(uint32_t samples_count, real_t* data_out) {
  const double start_ms = position_ms_;
  const uint32_t frames = samples_count;
  uint32_t data_offset = 0;
  while (true) {
    if (samples_to_next_event != 0xFFFFFFFF && samples_to_next_event <= samples_count) {
      /* generate samplesToNextEvent samples, process event and repeat */
      auto samples_to_generate = samples_to_next_event;
      if (samples_to_generate > 0) {
        GenerateIntoBufferStereo(samples_to_generate,data_out,data_offset);
        samples_count -= samples_to_generate;
        samples_to_next_event -= samples_to_generate;
      }
      if (last_event != nullptr)
        HandleEvent(last_event);
      last_event = GetNextEvent();
    } else {
      /* generate samples to end of buffer */
      if (samples_count > 0) {
        GenerateIntoBufferStereo(samples_count,data_out,data_offset);
        samples_to_next_event -= samples_count;
      }
      break;
    }
  }
  const double ms_per_frame = 1000.0 / sample_rate_ * speed_;
  position_ms_ = start_ms + frames * ms_per_frame;
  MixClicks(data_out, frames, start_ms, ms_per_frame);
}

// Metronome: a short decaying sine on every beat of the song's grid (higher on downbeats),
// added after the mixer.
void MidiSynth::StartClick(bool downbeat) {
  click_.phase = 0;
  click_.inc = float(6.283185307179586 * (downbeat ? 1760.0 : 1175.0) / sample_rate_);
  click_.amp = metronome_level_ * (downbeat ? 1.0f : 0.7f);
  click_.decay = expf(-1.0f / (0.012f * sample_rate_));
}

void MidiSynth::MixClicks(real_t* data_out, uint32_t frames, double start_ms, double ms_per_frame) {
  const bool on = metronome_on_;
  if (!on && click_.amp <= 1e-4f) return;
  const auto& beats = song_.beats;
  // first beat at or after start_ms
  size_t b = std::lower_bound(beats.begin(), beats.end(), start_ms,
                              [](const SongBeat& beat, double ms) { return beat.ms < ms; }) - beats.begin();
  for (uint32_t i = 0; i < frames; ++i) {
    const double t = start_ms + i * ms_per_frame;
    while (on && b < beats.size() && beats[b].ms < t + ms_per_frame) {  // beat within this frame
      StartClick(beats[b].beat == 0);
      ++b;
    }
    if (click_.amp > 1e-4f) {
      const float s = sinf(click_.phase) * click_.amp;
      click_.phase += click_.inc;
      click_.amp *= click_.decay;
      data_out[2 * i] += s;
      data_out[2 * i + 1] += s;
    }
  }
}


void MidiSynth::GenerateIntoBufferStereo(uint32_t samples_to_generate, real_t* data_out, uint32_t& data_offset) {
  // The internal mix buffers only hold buffers.main_size/2 stereo frames, but the player
  // can ask for more than that between two events, so render in buffer-sized chunks.
  const uint32_t max_frames = uint32_t(buffers.main_size / 2);
  while (samples_to_generate > max_frames) {
    GenerateChunkStereo(max_frames, data_out, data_offset);
    samples_to_generate -= max_frames;
  }
  if (samples_to_generate > 0)
    GenerateChunkStereo(samples_to_generate, data_out, data_offset);
}

void MidiSynth::GenerateChunkStereo(uint32_t samples_to_generate, real_t* data_out, uint32_t& data_offset) {
  //mix and send to aux
  MixChannelsStereo(samples_to_generate);
  //effects processing
  for (int i=0;i<5;++i) {
    if (main_effects[i] != nullptr)
      main_effects[i]->ProcessStereo(buffers.main,buffers.main,samples_to_generate);
    if (aux_effects[i] != nullptr)
      aux_effects[i]->ProcessStereo(buffers.aux,buffers.aux,samples_to_generate);
  }

  //mix with aux and send to output
  for (uint32_t i=0;i<samples_to_generate<<1;i+=2) {
    buffers.main[i] += buffers.aux[i];
    buffers.main[i+1] += buffers.aux[i+1];
    data_out[data_offset++] = buffers.main[i];
    data_out[data_offset++] = buffers.main[i+1];
  }
}

void MidiSynth::MixChannelsStereo(uint32_t samples_to_generate) {
  for (int i = 0; i < kChannelCount; ++i)
    channels[i]->RenderStereo(samples_to_generate);
  // Channel strips (filters, EQ, dynamics, fader, sends) and the master bus. With every
  // section off and faders at 0 dB this is exactly the plain sum of the channels.
  mixer.Process(buffers.channels, samples_to_generate, buffers.main);
  memset(buffers.aux, 0, sizeof(real_t) * samples_to_generate * 2);
}

midi::Event* MidiSynth::GetNextEvent() {
  midi::Event* result = nullptr;
  
  if (mode_ == kModeSequencer) {
    uint32_t track_index=0xFFFFFFFF;
    uint32_t event_index=0xFFFFFFFF;
    uint32_t ticks_to_next_event = 0xFFFFFFFF;

	  for (int i = 0; i < track_count_; i++) {
		  if (
			  tracks[i].ticks_to_next_event != 0xFFFFFFFF
			  && (ticks_to_next_event == 0xFFFFFFFF || tracks[i].ticks_to_next_event < ticks_to_next_event)
		  ) {
			  ticks_to_next_event = tracks[i].ticks_to_next_event;
			  track_index = i;
			  event_index = tracks[i].event_index;
		  }
	  }

	  if (track_index != 0xFFFFFFFF) {
		  /* consume event from that track */
		  result = &tracks[track_index].event_sequence[event_index];
		  if ((event_index+1) < tracks[track_index].event_count) {
			  tracks[track_index].ticks_to_next_event += tracks[track_index].event_sequence[event_index+1].deltaTime;
		  } else {
			  tracks[track_index].ticks_to_next_event = 0xFFFFFFFF;
		  }
		  ++tracks[track_index].event_index;
		  /* advance timings on all tracks by ticksToNextEvent */
		  for (int i = 0; i < track_count_; i++) {
			  if (tracks[i].ticks_to_next_event != 0xFFFFFFFF) {
				  tracks[i].ticks_to_next_event -= ticks_to_next_event;
			  }
		  }

	    auto beatsToNextEvent = ticks_to_next_event / double(ticks_per_beat);
		  auto secondsToNextEvent = beatsToNextEvent / (bpm / 60);
		  samples_to_next_event += uint32_t(secondsToNextEvent * sample_rate_ / speed_);
	  } else {
		  result = nullptr;
		  samples_to_next_event = 0xFFFFFFFF;
      /*if (player_->mode() == Player::kModeLoop) {
        player_->Reset();
      } else {
        player_->RequestStop();
      }*/
      if (!hold_at_end_) player_->Stop();  // while recording the transport keeps going
	  }
  } else if (mode_ == kModeTest) {
    EnterCriticalSection(&me_lock);
    result = &tracks[0].event_sequence[tracks[0].event_index];
    tracks[0].ticks_to_next_event = 0;

	  auto beatsToNextEvent = tracks[0].ticks_to_next_event / double(ticks_per_beat);
		auto secondsToNextEvent = beatsToNextEvent / (bpm / 60);
		samples_to_next_event += uint32_t(secondsToNextEvent * sample_rate_);
    tracks[0].event_index = (tracks[0].event_index + 1) % tracks[0].event_count;
    LeaveCriticalSection(&me_lock);
  }
  return result;
}

void MidiSynth::HandleEvent(midi::Event* event) {
  (this->*(midi_main_event_handlers[event->type]))(event);
};

void MidiSynth::MidiEventUnknown(midi::Event* event) {
  
}

void MidiSynth::MidiEventMeta(midi::Event* event) {
  (this->*(midi_meta_event_handlers[event->subtype]))(event);
}

void MidiSynth::MidiEventSetTempo(midi::Event* event) {
  bpm = 60000000.0 / event->data.tempo.microsecondsPerBeat;
}

void MidiSynth::MidiEventChannel(midi::Event* event) {
  (this->*(midi_channel_event_handlers[event->subtype]))(event);
}

void MidiSynth::MidiEventNoteOn(midi::Event* event) {
  if (!TrackAudible(event->track))
    return;  // muted track (its note-offs still go through)
  auto channel = channels[event->channel];
  real_t frequency = chromatic_scale_freq[(Notes)event->data.note.noteNumber];
  channel->AddNote(event->data.note.noteNumber,frequency,event->data.note.velocity / 127.0f);
  SetHeld(event->channel, event->data.note.noteNumber, true);
}

void MidiSynth::MidiEventNoteOff(midi::Event* event) {
  auto channel = channels[event->channel];
  channel->RemoveNote(event->data.note.noteNumber,event->data.note.velocity / 127.0f);
  SetHeld(event->channel, event->data.note.noteNumber, false);
}

void MidiSynth::MidiEventProgramChange(midi::Event* event) {
  const int ch = event->channel & 15;
  file_program_[ch] = ch == 9 ? 128 : event->data.program.number & 127;
  ApplyChannelProgram(ch);
  channels[ch]->set_silence(false);
}

void MidiSynth::set_program_override(int program) {
  program_override_ = program;
  if (initialized_ && program >= 0)
    for (int i = 0; i < kChannelCount; ++i)
      ApplyChannelProgram(i);
}

void MidiSynth::MidiEventController(midi::Event* event) {
  auto channel = channels[event->channel];
  const uint8_t value = event->data.controller.value;
  // param_coarse/param_fine hold the selected RPN (MSB/LSB); 127/127 = none.
  const bool bend_range_selected = channel->param_coarse == 0 && channel->param_fine == 0;
  switch (event->data.controller.type) {
    case 0x06:  // data entry MSB: pitch bend range in semitones
      if (bend_range_selected) {
        channel->des_coarse = value;
        channel->set_pitch_bend_range(real_t(channel->des_coarse) + channel->des_fine / 100.0f);
      }
      break;
    case 0x26:  // data entry LSB: cents
      if (bend_range_selected) {
        channel->des_fine = value;
        channel->set_pitch_bend_range(real_t(channel->des_coarse) + channel->des_fine / 100.0f);
      }
      break;
    case 0x07:  // channel volume
      channel->set_amplification(value / 127.0f);
      break;
    case 0x0A:  // pan
      channel->set_panning(value / 127.0f);
      break;
    case 0x0B:  // expression
      channel->set_expression(value / 127.0f);
      break;
    case 0x5B:  // effects 1 depth: reverb send (used when the mixer follows the file's sends)
      mixer.set_midi_send(event->channel, effects::Mixer::kBusReverb, value / 127.0f);
      break;
    case 0x5D:  // effects 3 depth: chorus send
      mixer.set_midi_send(event->channel, effects::Mixer::kBusChorus, value / 127.0f);
      break;
    case 0x40:  // sustain (damper) pedal
      channel->set_sustain(value >= 64);
      break;
    case 0x62: case 0x63:  // NRPN select: deselect RPN so data entry doesn't hit bend range
      channel->param_coarse = channel->param_fine = 127;
      break;
    case 0x64:  // RPN LSB
      channel->param_fine = value;
      break;
    case 0x65:  // RPN MSB
      channel->param_coarse = value;
      break;
    case 0x78:  // all sound off
    case 0x7B:  // all notes off
      channel->AllNotesOff();
      held_[event->channel & 15][0] = 0;
      held_[event->channel & 15][1] = 0;
      break;
    case 0x79:  // reset all controllers
      channel->ResetControllers();
      break;
  }
}

void MidiSynth::MidiEventPitchBend(midi::Event* event) {
  auto channel = channels[event->channel];
  // 14-bit value, 8192 = centre; full deflection = +-pitch_bend_range semitones.
  real_t amount = ((int(event->data.pitch.value) - 8192) / 8192.0f) * channel->pitch_bend_range();
  channel->SetPitchBend(amount);
}

}
}
}