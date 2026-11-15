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
      opt.detune_cents = 0.6f;
      opt.recorded_tuning = true;     // the piano's own stretched tuning
      opt.undamped_from_note = 89;    // top strings have no dampers (F6 up)
      grand = add(new instruments::CalibratedString(cal, opt));
      opt.detune_cents = 12.0f;
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
  drum_kit_ = add(ModalInstrument::NewDrumKit());

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
  for (int i = 0; i < kChannelCount; ++i)
    channel_program_[i] = i == 9 ? 128 : 0;  // 128 = drum kit

  tracks = nullptr;

  if (mode_ == kModeTest) {
    track_count_ = 1;
    tracks = new Track[track_count_];
    bpm = 120; //qn per min
    channels[0]->set_silence(false);
    uint32_t max_event_count = 0;
    for (int track_index=0;track_index<track_count_;++track_index) {
        auto& track = tracks[track_index];
        tracks[track_index].event_count = 20;
        tracks[track_index].event_sequence = new midi::Event[tracks[track_index].event_count];
        tracks[track_index].event_index = 0;
        max_event_count = max(max_event_count,tracks[track_index].event_count);
    }
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
  player_->Stop();
  
  midi::MidiLoader midifile;
  midifile.Load(data,data_size);
  
  DestroyTrackData();
  track_count_ = midifile.track_count;
  tracks = new Track[track_count_];
  ticks_per_beat = midifile.ticksPerBeat;
  bpm = 120; //qn per min
  double bps = bpm / 60.0;
  double secs_per_tick;
  double time_ms;
  secs_per_tick = 1 / ( bps * ticks_per_beat );
  player_->song_length_ms = 0;   


  uint32_t max_event_count = 0;
  for (int track_index=0;track_index<track_count_;++track_index) {
      auto& track = tracks[track_index];
      tracks[track_index].event_count = (uint32_t)midifile.eventList[track_index].size();
      tracks[track_index].event_sequence = new midi::Event[tracks[track_index].event_count];
      max_event_count = max(max_event_count,tracks[track_index].event_count);
  }
  //ResetTracks();
  //uint32_t abs_time[64];
  //memset(abs_time,0,sizeof(abs_time));
  
  for (uint32_t event_index=0;event_index<max_event_count;++event_index) {
    int track_index=0;
    for (track_index=0;track_index<track_count_;++track_index) {
      auto& track = tracks[track_index];
      if (event_index >= tracks[track_index].event_count)
        continue;
      auto source_event = &midifile.eventList[track_index][event_index];

      switch (source_event->subtype) {
        case midi::kEventSubtypeNoteOff:
          if (source_event->data.note.velocity == 0)
            source_event->data.note.velocity = 127;
        break;

        case midi::kEventSubtypeSetTempo:
          bpm = 60000000.0 / double(source_event->data.tempo.microsecondsPerBeat);
          bps = bpm / 60.0;
          secs_per_tick = 1 / ( bps * ticks_per_beat );
        break;

        case midi::kEventSubtypeProgramChange:

          if (source_event->channel == 9) {
            drum_kit_->Load();
          } else {
            instr[source_event->data.program.number]->Load();
          }
        break;
      }

      auto& current_event = tracks[track_index].event_sequence[event_index];
      memset(&current_event,0,sizeof(midi::Event));
      //abs_time[track_index] += source_event->deltaTime;
      double prev_abs_time_ms = event_index == 0?0.0:tracks[track_index].event_sequence[event_index-1].abs_time_ms;
      time_ms = secs_per_tick * 1000 * source_event->deltaTime;
      current_event.abs_time_ms = prev_abs_time_ms+time_ms;
      current_event.deltaTime = source_event->deltaTime;
      current_event.type = source_event->type;
      current_event.subtype = source_event->subtype;
      current_event.channel = source_event->channel;
      current_event.track = source_event->track;
      channels[source_event->channel]->set_silence(false);
      memcpy(&current_event.data,&source_event->data,sizeof(current_event.data));
      
      #ifdef OUTPUT_EVENTS
        char str[512];
        sprintf(str,"time\t%04.03f\ttype : %s\t%02d\t%s\ttrack %02d\tchannel %02d\n",time_ms,eventnames[source_event->type],source_event->subtype,subeventnames[source_event->subtype],source_event->track,source_event->channel);
        OutputDebugString(str);
      #endif
      if (current_event.subtype == midi::kEventSubtypeProgramChange) {
        char str[512];
        sprintf_s(str,"time %04.03f type : %s %s track %d channel %d",time_ms,eventnames[source_event->type],subeventnames[source_event->subtype],source_event->track,source_event->channel);
        OutputDebugString(str);
        sprintf_s(str,"[data: %d]\n",current_event.data.program.number);
        OutputDebugString(str);
      }
      /*if (current_event.subtype == midi::kEventSubtypeController) {
        sprintf(str,"time %04.03f type : %s %s track %d channel %d\n",time_ms,eventnames[source_event->type],eventnames[source_event->subtype],source_event->track,source_event->channel);
        OutputDebugString(str);
        sprintf(str,"\tdata: %d %d\n",current_event.data.controller.type,current_event.data.controller.value);
        OutputDebugString(str);
      }*/
    }
    
  } 

  // Absolute event times and song length. Tempo changes can live on any track (usually
  // track 0) and apply to all tracks from their tick onwards, so build a time-ordered tempo
  // map first, then convert each event's absolute tick to milliseconds through it.
  {
    struct TempoChange { uint64_t tick; uint32_t us_per_beat; };
    std::vector<TempoChange> tempo_map;
    for (int t = 0; t < track_count_; ++t) {
      uint64_t tick = 0;
      for (uint32_t e = 0; e < tracks[t].event_count; ++e) {
        auto& ev = tracks[t].event_sequence[e];
        tick += ev.deltaTime;
        if (ev.subtype == midi::kEventSubtypeSetTempo && ev.data.tempo.microsecondsPerBeat != 0)
          tempo_map.push_back({tick, ev.data.tempo.microsecondsPerBeat});
      }
    }
    std::stable_sort(tempo_map.begin(), tempo_map.end(),
                     [](const TempoChange& a, const TempoChange& b) { return a.tick < b.tick; });

    const double tpb = ticks_per_beat ? double(ticks_per_beat) : 480.0;
    auto tick_to_ms = [&](uint64_t tick) {
      double ms = 0;
      uint64_t last_tick = 0;
      uint32_t us_per_beat = 500000;  // 120 bpm until the first tempo event
      for (auto& tc : tempo_map) {
        if (tc.tick > tick) break;
        ms += double(tc.tick - last_tick) * us_per_beat / (1000.0 * tpb);
        last_tick = tc.tick;
        us_per_beat = tc.us_per_beat;
      }
      return ms + double(tick - last_tick) * us_per_beat / (1000.0 * tpb);
    };

    player_->song_length_ms = 0;
    for (int t = 0; t < track_count_; ++t) {
      uint64_t tick = 0;
      for (uint32_t e = 0; e < tracks[t].event_count; ++e) {
        auto& ev = tracks[t].event_sequence[e];
        tick += ev.deltaTime;
        ev.abs_time_ms = tick_to_ms(tick);
        // Ignore trailing meta events (end-of-track can carry a long delta) for the length.
        if (ev.type == midi::kEventTypeChannel)
          player_->song_length_ms = max(player_->song_length_ms, ev.abs_time_ms);
      }
    }

    // Tempo map and bar/beat grid for the UI.
    song_ = SongInfo();
    song_.length_ms = player_->song_length_ms;
    if (tempo_map.empty() || tempo_map[0].tick != 0)
      song_.tempo.push_back({0.0, 120.0});
    for (auto& tc : tempo_map)
      song_.tempo.push_back({tick_to_ms(tc.tick), 60000000.0 / tc.us_per_beat});
    struct Signature { uint64_t tick; int num, den; };
    std::vector<Signature> sigs;
    uint64_t last_tick = 0;
    for (int t = 0; t < track_count_; ++t) {
      uint64_t tick = 0;
      for (uint32_t e = 0; e < tracks[t].event_count; ++e) {
        auto& ev = tracks[t].event_sequence[e];
        tick += ev.deltaTime;
        if (ev.type == midi::kEventTypeChannel) last_tick = max(last_tick, tick);
        if (ev.type == midi::kEventTypeMeta && ev.subtype == midi::kEventSubtypeTimeSignature &&
            ev.data.time_signature.numerator > 0 && ev.data.time_signature.denominator > 0)
          sigs.push_back({tick, ev.data.time_signature.numerator, ev.data.time_signature.denominator});
      }
    }
    std::stable_sort(sigs.begin(), sigs.end(), [](const Signature& a, const Signature& b) { return a.tick < b.tick; });
    int num = 4, den = 4, bar = 1, beat = 0;
    size_t next_sig = 0;
    uint64_t tick = 0;
    while (tick <= last_tick + uint64_t(tpb) * 4 && song_.beats.size() < 100000) {
      while (next_sig < sigs.size() && sigs[next_sig].tick <= tick) {
        num = sigs[next_sig].num;
        den = sigs[next_sig].den;
        if (beat != 0) { ++bar; beat = 0; }  // a new signature starts a new bar
        ++next_sig;
      }
      song_.beats.push_back({tick_to_ms(tick), bar, beat});
      const uint64_t step = uint64_t(tpb * 4 / den);
      tick += step ? step : 1;
      if (++beat >= num) { beat = 0; ++bar; }
    }
  }
  BuildSongInfo(midifile);
  track_mute_mask_ = 0;
  track_solo_mask_ = 0;

  if (program_override_ >= 0)
    set_program_override(program_override_);  // songs without program changes start on it too
  Reset();
}

void MidiSynth::BuildSongInfo(const midi::MidiLoader& file) {
  song_.tracks.resize(track_count_);
  for (int t = 0; t < track_count_; ++t) {
    SongTrack& st = song_.tracks[t];
    if (t < 64) st.name = file.track_names[t];
    float start[16][128];
    uint8_t velocity[16][128];
    for (auto& row : start) for (auto& v : row) v = -1.0f;
    double last_ms = 0;
    auto close = [&](int ch, int note, double end_ms) {
      if (start[ch][note] < 0) return;
      st.notes.push_back({start[ch][note], float(end_ms), uint8_t(note), velocity[ch][note], uint8_t(ch)});
      start[ch][note] = -1.0f;
    };
    for (uint32_t e = 0; e < tracks[t].event_count; ++e) {
      const auto& ev = tracks[t].event_sequence[e];
      if (ev.type != midi::kEventTypeChannel) continue;
      const int ch = ev.channel & 15;
      last_ms = ev.abs_time_ms;
      st.channel_mask |= uint16_t(1 << ch);
      switch (ev.subtype) {
        case midi::kEventSubtypeNoteOn: {
          const int note = ev.data.note.noteNumber & 127;
          close(ch, note, ev.abs_time_ms);  // re-struck without a note-off
          start[ch][note] = float(ev.abs_time_ms);
          velocity[ch][note] = ev.data.note.velocity;
          break;
        }
        case midi::kEventSubtypeNoteOff:
          close(ch, ev.data.note.noteNumber & 127, ev.abs_time_ms);
          break;
        case midi::kEventSubtypeProgramChange:
          if (st.program < 0) st.program = ch == 9 ? 128 : ev.data.program.number;
          break;
        default:
          break;
      }
    }
    for (int ch = 0; ch < 16; ++ch)
      for (int n = 0; n < 128; ++n) close(ch, n, last_ms);
    std::sort(st.notes.begin(), st.notes.end(), [](const SongNote& a, const SongNote& b) { return a.start_ms < b.start_ms; });
    for (auto& n : st.notes) {
      if (n.note < st.low_note) st.low_note = n.note;
      if (n.note > st.high_note) st.high_note = n.note;
    }
    if (st.program < 0 && (st.channel_mask & (1 << 9))) st.program = 128;
  }
  song_.loaded = true;
}

void MidiSynth::ResetChannelsToDefaults() {
  for (int i = 0; i < kChannelCount; ++i) {
    auto c = channels[i];
    if (i == 9) {
      c->set_instrument(drum_kit_);
      channel_program_[i] = 128;
    } else {
      const int program = program_override_ >= 0 ? program_override_ : 0;
      c->set_instrument(instr[program & 127]);
      channel_program_[i] = program;
    }
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
    if (ev->abs_time_ms >= target_ms) {
      last_event = ev;
      samples_to_next_event = uint32_t((ev->abs_time_ms - target_ms) * 0.001 * sample_rate_);
      break;
    }
    if (ev->subtype != midi::kEventSubtypeNoteOn && ev->subtype != midi::kEventSubtypeNoteOff)
      HandleEvent(ev);
  }
}


void MidiSynth::RenderIdleStereo(uint32_t samples_count, real_t* data_out) {
  uint32_t data_offset = 0;
  GenerateIntoBufferStereo(samples_count, data_out, data_offset);
}

void MidiSynth::AllNotesOff() {
  for (int i=0;i<kChannelCount;++i) {
    if (channels[i] != nullptr) {
      channels[i]->set_sustain(false);  // a pedal left down by the file would hold new notes
      channels[i]->AllNotesOff();
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

void MidiSynth::RenderSamplesStereo(uint32_t samples_count, real_t* data_out) {
	uint32_t data_offset = 0;
  if (mode_ == kModeTest) {
    samples_to_next_event = samples_count;
		auto samples_to_generate = samples_to_next_event;
		if (samples_to_generate > 0) {
      GenerateIntoBufferStereo(samples_to_generate,data_out,data_offset);
			data_offset += samples_to_generate * 2;
			samples_count -= samples_to_generate;
			samples_to_next_event -= samples_to_generate;
      if (mode_ == kModeTest)
        samples_to_next_event = 0xFFFFFFFF;
		}
    if (last_event != nullptr) {
			HandleEvent(last_event);
      memset(last_event,0,sizeof(midi::Event));
    }
    last_event = GetNextEvent();
  } else if (mode_ == kModeSequencer) {
    while (true) {
		  if (samples_to_next_event != 0xFFFFFFFF && samples_to_next_event <= samples_count) {
			  /* generate samplesToNextEvent samples, process event and repeat */
			  auto samples_to_generate = samples_to_next_event;
			  if (samples_to_generate > 0) {
          GenerateIntoBufferStereo(samples_to_generate,data_out,data_offset);
				  samples_count -= samples_to_generate;
				  samples_to_next_event -= samples_to_generate;
          if (mode_ == kModeTest)
            samples_to_next_event = 0xFFFFFFFF;
			  }
        if (last_event != nullptr) {
			    HandleEvent(last_event);
          if (mode_ == kModeTest)
            memset(last_event,0,sizeof(midi::Event));
        }
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
		  samples_to_next_event += uint32_t(secondsToNextEvent * sample_rate_);
	  } else {
		  result = nullptr;
		  samples_to_next_event = 0xFFFFFFFF;
      /*if (player_->mode() == Player::kModeLoop) {
        player_->Reset();
      } else {
        player_->RequestStop();
      }*/
      player_->Stop();
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
}

void MidiSynth::MidiEventNoteOff(midi::Event* event) {
  auto channel = channels[event->channel];
  channel->RemoveNote(event->data.note.noteNumber,event->data.note.velocity / 127.0f);
}

void MidiSynth::MidiEventProgramChange(midi::Event* event) {
  auto channel = channels[event->channel];
  if (event->channel == 9) {
    channel->set_instrument(drum_kit_);
  } else {
    int program = program_override_ >= 0 ? program_override_ : event->data.program.number;
    channel->set_instrument(instr[program & 127]);
    channel_program_[event->channel] = program & 127;
  }
  channel->set_silence(false);
}

void MidiSynth::set_program_override(int program) {
  program_override_ = program;
  if (initialized_ && program >= 0)
    for (int i = 0; i < kChannelCount; ++i)
      if (i != 9) {
        channels[i]->set_instrument(instr[program & 127]);
        channel_program_[i] = program & 127;
      }
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