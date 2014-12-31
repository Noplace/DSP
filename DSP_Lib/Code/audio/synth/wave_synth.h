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

#pragma pack(push, 1)
struct WaveHeader {
  char chunkid[4];
  uint32_t chunksize;
  char format[4];
  char   Subchunk1ID[4];
  uint32_t  Subchunk1Size;
  uint16_t  AudioFormat;
  uint16_t  NumChannels;
  uint32_t  SampleRate;
  uint32_t  ByteRate;
  uint16_t  BlockAlign;
  uint16_t  BitsPerSample;
  char Subchunk2ID[4];
  uint32_t Subchunk2Size;
};
#pragma pack(pop)

class WaveSynth : public Synth {
 public:

  enum State {
    kStateStopped=0,kStatePlaying=1,kStatePaused=2
  };

  WaveSynth() : Synth(),initialized_(false),samples_to_next_event(0) {


  }
  ~WaveSynth() {
    Deinitialize();
  }
  void Initialize();
  void Deinitialize();
  void RenderSamplesStereo(uint32_t samples_count, real_t* data_out);
  void Reset();

  void LoadWaveFromFile(const char* filename);
  void LoadWave(uint8_t* data, size_t data_size);

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

  WaveHeader wave_header_;
  uint8_t* wave_data_;
};

}
}
}