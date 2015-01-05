/*****************************************************************************************************************
* Copyright (c) 2015 Khalid Ali Al-Kooheji                                                                       *
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
namespace formats {

class SPCPlayer : public GeneralPlayer {
 public:
  double song_length_ms;
  SPCPlayer() : GeneralPlayer() {
  }
  virtual ~SPCPlayer() {
    
  }
  void Initialize() {
    GeneralPlayer::Initialize();
    song_counter_ms = 0;
    auto sample_rate = audio_interface_->wave_format().nSamplesPerSec;

    {
      output_buffer_samples_ = audio_interface_->buffer_size()/sizeof(short); //size in samples
      output_buffer_length_ms_ = output_buffer_samples_ / double(sample_rate*audio_interface_->wave_format().nChannels*0.001); //400.0;//400ms
      output_buffer = new short[output_buffer_samples_]; 
      mix_buffer = new real_t[output_buffer_samples_]; 
    }

    synth_.set_sample_rate(sample_rate);
    synth_.Initialize();
  }
  virtual void Deinitialize() {
    synth_.Deinitialize();
    SafeDeleteArray(&mix_buffer);
    SafeDeleteArray(&output_buffer);
  }

  void OnPause() {
    memset(output_buffer,0,output_buffer_samples_*sizeof(short));
    audio_interface_->Write(output_buffer,output_buffer_samples_*sizeof(short));
  }
  void OnPlay() {
  }
  void OnStop() {
    synth_.Reset();
    song_counter_ms = 0;
  }
  void LoadFromFile(const char* filename) {
    Stop();
    synth_.LoadFromFile(filename);
  }
  void LoopFunc() {
   synth_.Step(); 
  }
 private:
  SPCSynth synth_;
  real_t* mix_buffer;
  short* output_buffer;
  double song_pos_ms,song_counter_ms,output_buffer_length_ms_;
  uint32_t output_buffer_samples_;


};

}
}
}