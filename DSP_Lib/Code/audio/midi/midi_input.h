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

#include <atomic>
#include <memory>
#include <mmsystem.h>

namespace dsp {
namespace audio {
namespace midi {

// Lock-free single-producer / single-consumer ring of packed short MIDI messages
// (status | data1 << 8 | data2 << 16), the same packing winmm uses.
template <uint32_t N>
class SpscMessageQueue {
 public:
  SpscMessageQueue() : head_(0), tail_(0), dropped_(0) {}
  // Producer side. Returns false (and counts a drop) if the queue is full.
  bool Push(uint32_t message) {
    const uint32_t head = head_.load(std::memory_order_relaxed);
    const uint32_t next = (head + 1) % N;
    if (next == tail_.load(std::memory_order_acquire)) {
      dropped_.fetch_add(1, std::memory_order_relaxed);
      return false;
    }
    buffer_[head] = message;
    head_.store(next, std::memory_order_release);
    return true;
  }
  // Consumer side.
  bool Pop(uint32_t& message) {
    const uint32_t tail = tail_.load(std::memory_order_relaxed);
    if (tail == head_.load(std::memory_order_acquire))
      return false;
    message = buffer_[tail];
    tail_.store((tail + 1) % N, std::memory_order_release);
    return true;
  }
  uint32_t dropped() const { return dropped_.load(std::memory_order_relaxed); }

 private:
  uint32_t buffer_[N];
  std::atomic<uint32_t> head_, tail_, dropped_;
};

inline uint32_t PackShortMessage(uint8_t status, uint8_t data1, uint8_t data2) {
  return uint32_t(status) | (uint32_t(data1) << 8) | (uint32_t(data2) << 16);
}

// Live MIDI input from every connected input port, via winmm (midiIn*).
// On Windows 11 with Windows MIDI Services installed, winmm ports are routed through the new
// MIDI stack and can be shared with other apps.
//
// Each port's driver callback pushes into its own SPSC queue; the audio thread calls Drain()
// once per buffer. OpenAll()/CloseAll() must not run concurrently with Drain() (RealtimePlayer
// does both under its synth lock).
class MidiInput {
 public:
  static const int kMaxPorts = 16;

  MidiInput();
  ~MidiInput();

  // Opens and starts every input port that isn't already in use. Returns how many opened.
  int OpenAll();
  void CloseAll();

  int available_count() const { return available_; }  // ports reported by the system
  int open_count() const { return open_; }
  const char* port_name(int i) const { return ports_[i] ? ports_[i]->name : ""; }
  uint32_t dropped_messages() const;

  // Audio thread: hand every queued message to f(uint32_t packed_message).
  template <typename F>
  void Drain(F&& f) {
    for (int i = 0; i < open_; ++i) {
      uint32_t message;
      while (ports_[i]->queue.Pop(message))
        f(message);
    }
  }

 private:
  struct Port {
    HMIDIIN handle = nullptr;
    char name[MAXPNAMELEN] = {};
    SpscMessageQueue<1024> queue;
  };
  static void CALLBACK InputProc(HMIDIIN handle, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR param2);

  std::unique_ptr<Port> ports_[kMaxPorts];
  int open_;
  int available_;
};

}
}
}
