/*****************************************************************************************************************
* Copyright (c) 2014 Khalid Ali Al-Kooheji                                                                       *
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

#include "spc/spc700.h"
#include "spc/dsp.h"

namespace dsp {
namespace audio {
namespace formats {

#pragma pack(push, 1)
struct SPCHeader {
  char header[33];
  uint16_t unused1;
  uint8_t id666_tag;
  uint8_t minor_version;
  uint16_t PC;
  uint8_t A;
  uint8_t X;
  uint8_t Y;
  uint8_t PSW;
  uint8_t SP_low;
  uint16_t reserved1;
  struct {
    char song_title[32];
    char game_title[32];
    char dumper_name[16];
    char comments[32];
    char dump_date[11];
    char seconds_to_play[3]; //before fading out
    char fade_out_ms[5]; //before fading out
    char artist[32];
    char channel_disable;
    char emulator_used;
  } text;
  char reserved2[45];
  struct {
    char song_title[32];
    char game_title[32];
    char dumper_name[16];
    char comments[32];
    uint32_t dump_date;
    char unused1[7];
    char seconds_to_play[3]; //before fading out
    uint32_t fade_out_ms; //before fading out
    char artist[32];
    char channel_disable;
    char emulator_used;
  } bin;
  char reserved3[46];
};



#pragma pack(pop)

class SPCSynth : public synth::Synth {
 public:

  enum State {
    kStateStopped=0,kStatePlaying=1,kStatePaused=2
  };

  SPCSynth() : Synth(),initialized_(false),samples_to_next_event(0) {


  }
  ~SPCSynth() {
    Deinitialize();
  }
  void Initialize();
  void Deinitialize();
  void RenderSamplesStereo(uint32_t samples_count, real_t* data_out);
  void Reset();

  void LoadFromFile(const char* filename);
  void Load(uint8_t* data, size_t data_size);

 private:
  struct Track {
    midi::Event* event_sequence;
    uint32_t event_index,event_count,ticks_to_next_event;
    __forceinline midi::Event* GetCurrentEvent() {
      return &event_sequence[event_index];
    }
  };

  void GenerateIntoBufferStereo(uint32_t samples_to_generate, real_t* data_out, uint32_t& data_offset);
  void MixChannelsStereo(uint32_t samples_to_generate);

  CRITICAL_SECTION me_lock;
  uint32_t samples_to_next_event;
  bool initialized_;

  SPCHeader header_;
  uint8_t* ram_;
  uint8_t* dsp_registers_;
  spc::SPC700 spc700;
};

}
}
}