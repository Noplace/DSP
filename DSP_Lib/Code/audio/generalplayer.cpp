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
#include "../dsp.h"


#define WM_SP_QUIT 0x400
#define WM_SP_PLAY 0x401
#define WM_SP_PAUSE 0x402
#define WM_SP_STOP 0x403

#define SQRT_1OVER2  0.70710678118654752440084436210485

namespace dsp {
namespace audio {


static void clamp(void *dest, const float *src, int32_t nsamples)
{
  int16_t *dests = (int16_t *)dest;

  for (int32_t i=0; i < nsamples; i++)
  {
    float v = src[i] * 32768.0f;
    if (v >  32600.0f) v =  32600.0f;
    if (v < -32600.0f) v = -32600.0f;
    dests[i] = (int16_t)v;
  }
}




void GeneralPlayer::Initialize() {
  if (initialized_ == true)
    return;

  state_ = kStateStopped;
 

  thread_msg = 0;
  InitializeCriticalSectionAndSpinCount(&cs,0x100);
  player_event = CreateEvent(NULL,FALSE,FALSE,NULL);
  thread_handle = CreateThread(nullptr,0,static_cast<LPTHREAD_START_ROUTINE>(PlayThread),this,0,(LPDWORD)&thread_id);//CREATE_SUSPENDED
  if (thread_handle != nullptr) {
    SetThreadPriority(thread_handle,THREAD_PRIORITY_ABOVE_NORMAL);
  }

  initialized_ = true;
}

void GeneralPlayer::Deinitialize() {
  if (initialized_ == false)
    return;
  Stop();
  //force close
  SetEvent(player_event);
  SendThreadMessage(WM_SP_QUIT);
  WaitForSingleObject(thread_handle,INFINITE);
  CloseHandle(thread_handle);
  CloseHandle(player_event);
  DeleteCriticalSection(&cs);




  initialized_ = false;
}

void GeneralPlayer::Play() {
  if (state_ == kStatePlaying) return;

  auto output_sr = audio_interface_->wave_format().nSamplesPerSec;
 // auto input_sr = synth_->sample_rate_;

  SetEvent(player_event);
  SendThreadMessage(WM_SP_PLAY);
}

void GeneralPlayer::Pause() {
  if (state_ == kStatePaused) return;
  SendThreadMessage(WM_SP_PAUSE);
}

void GeneralPlayer::Stop() {
  if (state_ == kStateStopped) return;
  SendThreadMessage(WM_SP_STOP);
}


DWORD WINAPI GeneralPlayer::PlayThread(LPVOID lpThreadParameter) {
  return static_cast<GeneralPlayer*>(lpThreadParameter)->InstancePlayThread();
}

DWORD GeneralPlayer::InstancePlayThread() {
  Timer timer;

  uint64_t current_cycles=0,prev_cycles=0;
  double span_accumulator = 0;
  double update_time = 1000.0 / 2048000.0;
  double timer_res = timer.resolution();
  //uint32_t samples_to_render = uint32_t(update_time * 0.001 * audio_interface_->wave_format().nSamplesPerSec);
  //uint32_t buffer_size = samples_to_render * audio_interface_->wave_format().nBlockAlign;

  thread_time_span = 0;
  while (1) {
    EnterCriticalSection(&cs);
    if (thread_msg == WM_SP_QUIT) {
        thread_msg = 0;
        break;
    } else if (thread_msg == WM_SP_PLAY) {
      if (state_ != kStatePlaying) {
        prev_cycles = timer.GetCurrentCycles();
        state_ = kStatePlaying;
        span_accumulator = update_time;
        audio_interface_->Play();
        OnPlay();
      }
    } else if (thread_msg == WM_SP_PAUSE) {
      if (state_ != kStatePaused) {
        ResetEvent(player_event);
        state_ = kStatePaused;
        
        OnPause();
      }
    } else if (thread_msg == WM_SP_STOP) {
      if (state_ != kStateStopped) {
        ResetEvent(player_event);
        state_ = kStateStopped;
        audio_interface_->Stop();
        thread_time_span = 0;
        OnStop();
      }
    } 
    thread_msg = 0;
    LeaveCriticalSection(&cs);

    if (state_ == kStatePlaying) {
      current_cycles = timer.GetCurrentCycles();
      double time_span =  (current_cycles - prev_cycles) * timer_res;
      if (time_span >= 500.0) //should be at max, output buffer length in ms
        time_span = 500.0;
 
      while (span_accumulator >= update_time) {
        //memset(mix_buffer,0,output_buffer_samples_*sizeof(real_t));
        LoopFunc();
        span_accumulator -= update_time;
      }
      span_accumulator += time_span;
      thread_time_span += time_span;
      if (thread_time_span>=1000.0) {
        WaitForSingleObject(player_event,10);
        thread_time_span = 0;
      }
      prev_cycles = current_cycles;
    } else {
      //wait for pause/stop events with timer
      WaitForSingleObject(player_event,2000);
    }
  }

  return S_OK;
}


}
}