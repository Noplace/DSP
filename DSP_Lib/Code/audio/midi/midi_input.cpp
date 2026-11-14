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
#include <stdio.h>
#include "../../dsp.h"

#pragma comment(lib, "winmm.lib")

namespace dsp {
namespace audio {
namespace midi {

MidiInput::MidiInput() : open_(0), available_(0) {
}

MidiInput::~MidiInput() {
  CloseAll();
}

int MidiInput::OpenAll() {
  CloseAll();
  available_ = (int)midiInGetNumDevs();
  for (UINT id = 0; id < (UINT)available_ && open_ < kMaxPorts; ++id) {
    auto port = std::make_unique<Port>();
    MIDIINCAPSA caps = {};
    if (midiInGetDevCapsA(id, &caps, sizeof(caps)) == MMSYSERR_NOERROR)
      strncpy_s(port->name, caps.szPname, _TRUNCATE);
    else
      sprintf_s(port->name, "MIDI In %u", id);

    MMRESULT r = midiInOpen(&port->handle, id, (DWORD_PTR)&InputProc, (DWORD_PTR)port.get(), CALLBACK_FUNCTION);
    if (r != MMSYSERR_NOERROR) {
      char text[200];
      sprintf_s(text, "MIDI: could not open input '%s' (error %u, in use by another app?)\n", port->name, r);
      OutputDebugStringA(text);
      continue;
    }
    if (midiInStart(port->handle) != MMSYSERR_NOERROR) {
      midiInClose(port->handle);
      continue;
    }
    char text[200];
    sprintf_s(text, "MIDI: listening on '%s'\n", port->name);
    OutputDebugStringA(text);
    ports_[open_++] = std::move(port);
  }
  return open_;
}

void MidiInput::CloseAll() {
  for (int i = 0; i < open_; ++i) {
    midiInStop(ports_[i]->handle);
    midiInReset(ports_[i]->handle);
    midiInClose(ports_[i]->handle);  // no callbacks after this returns
    ports_[i].reset();
  }
  open_ = 0;
}

uint32_t MidiInput::dropped_messages() const {
  uint32_t total = 0;
  for (int i = 0; i < open_; ++i)
    total += ports_[i]->queue.dropped();
  return total;
}

void CALLBACK MidiInput::InputProc(HMIDIIN, UINT msg, DWORD_PTR instance, DWORD_PTR param1, DWORD_PTR) {
  // Driver callback: keep it tiny, no system calls besides what the queue does.
  if (msg != MIM_DATA)
    return;  // ignore open/close/sysex/errors
  const uint32_t message = (uint32_t)param1;
  const uint8_t status = message & 0xFF;
  if (status == 0xF8 || status == 0xFE)
    return;  // timing clock / active sensing: high rate, not needed
  reinterpret_cast<Port*>(instance)->queue.Push(message);
}

}
}
}
