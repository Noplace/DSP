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
#include "../../dsp.h"

namespace dsp {
namespace audio {
namespace synth {


void WaveSynth::Initialize() {
  if (initialized_ == true)
    return;

  wave_data_ = nullptr;
  
  InitializeCriticalSection(&me_lock);
  initialized_ = true;
}

void WaveSynth::Deinitialize() {
  if (initialized_ == false)
    return;
  DeleteCriticalSection(&me_lock);
  SafeDeleteArray(&wave_data_);
  initialized_ = false;
}

void WaveSynth::Reset() {

}


void WaveSynth::LoadWaveFromFile(const char* filename) {
  uint8_t* buffer;
  size_t size=0;
  io::ReadWholeFileBinary(filename,&buffer,size);
  if (size != 0) {
    LoadWave(buffer,size);
    io::DestroyFileBuffer(&buffer);
  }
}

void WaveSynth::LoadWave(uint8_t* data, size_t data_size) {
  player_->Stop();
  SafeDeleteArray(&wave_data_);
  memcpy(&wave_header_,data,sizeof(wave_header_));
  data += sizeof(wave_header_);
  if (strncmp(wave_header_.chunkid,"RIFF",4)!=0)
    return;
 
  sample_rate_  = wave_header_.SampleRate;
  wave_data_ = new uint8_t[wave_header_.Subchunk2Size];
  memcpy(wave_data_,data,wave_header_.Subchunk2Size);



  Reset();
}


void WaveSynth::RenderSamplesStereo(uint32_t samples_count, real_t* data_out) {
	uint32_t data_offset = 0;


		/* generate samples to end of buffer */
		if (samples_count > 0) {
			GenerateIntoBufferStereo(samples_count,data_out,data_offset);
			samples_to_next_event -= samples_count;
		}

  


}


void WaveSynth::GenerateIntoBufferStereo(uint32_t samples_to_generate, real_t* data_out, uint32_t& data_offset) {
  //mix and send to aux
  //MixChannelsStereo(samples_to_generate);

  /*static uint32_t t = 0;
  for (uint32_t i=0;i<samples_to_generate;++i) {
    float sample = 0.2f*sin(2.0f*XM_PI*(t/real_t(sample_rate_))*220.0f);
    data_out[data_offset++] = sample;
    data_out[data_offset++] = sample;
    t = (t + 1) % 44100;
  }*/

  static uint32_t total_offset=0;
  static uint32_t samples_count = 0;
  uint32_t total_samples = wave_header_.Subchunk2Size / wave_header_.BlockAlign;
  int16_t* ptr16bit = (int16_t*)&wave_data_[total_offset];
  uint32_t newbytes = samples_to_generate*wave_header_.BlockAlign;

  total_offset += newbytes;

  if ((samples_count+samples_to_generate) >= total_samples) {
    samples_to_generate = total_samples - samples_count;
  }
  samples_count += samples_to_generate;
  
  for (uint32_t i=0;i<samples_to_generate;++i) {
    float left = *ptr16bit++ / (32768.0f);
    float right = *ptr16bit++ / (32768.0f);
    data_out[data_offset++] = 0.5f*left;
    data_out[data_offset++] = 0.5f*right;
  }
  
  if (samples_count == total_samples) {
    player_->Stop();
    return;
  }

  /*//mix with aux and send to output
  for (uint32_t i=0;i<samples_to_generate<<1;i+=2) {
    buffers.main[i] += buffers.aux[i];
    buffers.main[i+1] += buffers.aux[i+1];
    data_out[data_offset++] = buffers.main[i];
    data_out[data_offset++] = buffers.main[i+1];
  }*/
}

void WaveSynth::MixChannelsStereo(uint32_t samples_to_generate) {
  
    

}


}
}
}