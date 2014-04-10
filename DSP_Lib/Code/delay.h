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

namespace dsp {

class Delay : public ProcessingComponent {
 public:
  Delay() : ProcessingComponent(), max_samples_(0), buffer_index_(0),delay_ms_(200.0f),feedback_(0.3f) {
  }
  ~Delay() {
    Deinitialize();
  }

  void Initialize(int max_samples)  {
    Deinitialize();
    max_samples_ = max_samples;
    buffer_ = new real_t[max_samples];
    for (auto i=0;i<max_samples;++i)
      buffer_[i] = 0;
    //memset(buffer_,0,max_samples*sizeof(real_t));
  }

  void Deinitialize() {
    SafeDeleteArray(&buffer_);
  }

  real_t Tick(real_t sample) {
      buffer_[buffer_index_] = sample;
      unsigned delayed_index = ( buffer_index_ - delay_index_ + max_samples_ ) % max_samples_;
      auto output = buffer_[buffer_index_] = (sample + (buffer_[delayed_index]*feedback_));
      buffer_index_ = (buffer_index_ + 1) % max_samples_;
  }
 
  void set_feedback(real_t feedback) { feedback_ = feedback; }
  void set_delay_ms(real_t delay_ms) { 
    delay_ms_ = delay_ms; 
    delay_index_ = int( delay_ms_ * 0.001f * sample_rate_ ); 
  }
 private:
  real_t* buffer_;
  real_t feedback_,delay_ms_;
  int buffer_index_,max_samples_;
  int delay_index_;
};

}


