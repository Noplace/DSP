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
namespace audio {
namespace formats {
namespace spc {

struct BRR {

};

class DSP {
 public:
  void Reset();
  uint8_t Read(uint8_t addr);
  void Write(uint8_t addr, uint8_t value);
  uint8_t* regptr() { return regs; }
  void set_mem(uint8_t* mem) { mem_ = mem; }
  short Tick(); //16bit samples originally at 32khz 
 private:
  uint8_t* mem_;
  struct ADSR_reg {
    uint8_t AR:4;
    uint8_t DR:3;
    uint8_t adsr_or_gain:1;
    uint8_t SR:4;
    uint8_t SL:4;
  };
  struct VoiceChannel {
      int8_t vol_l;
      int8_t vol_r;
      uint8_t pitch_h;
      uint8_t pitch_l;
      uint16_t pitch() { return ((pitch_h<<8)|pitch_l)&0x3FFF;}
      ADSR_reg adsr;
      uint8_t gain;
      uint8_t scrn;
      uint8_t envx;
      uint8_t outx;
  };
  union {
    struct {
      VoiceChannel voice0;
      uint8_t _0a;
      uint8_t _0b;
      int8_t main_vol_l;
      uint8_t efb;
      uint8_t _0e;
      uint8_t C0;
      VoiceChannel voice1;
      uint8_t _1a;
      uint8_t _1b;
      int8_t main_vol_r;
      uint8_t _1d;
      uint8_t _1e;
      uint8_t C1;
      VoiceChannel voice2;
      uint8_t _2a;
      uint8_t _2b;
      int8_t echo_vol_l;
      uint8_t pmon;
      uint8_t _2e;
      uint8_t C2;
      VoiceChannel voice3;
      uint8_t _3a;
      uint8_t _3b;
      int8_t echo_vol_r;
      uint8_t nov;
      uint8_t _3e;
      uint8_t _C3;
      VoiceChannel voice4;
      uint8_t _4a;
      uint8_t _4b;
      uint8_t kon;
      uint8_t eov;
      uint8_t _4e;
      uint8_t C4;
      VoiceChannel voice5;
      uint8_t _5a;
      uint8_t _5b;
      uint8_t koff;
      uint8_t dir;
      uint8_t _5e;
      uint8_t C5;
      VoiceChannel voice6;
      uint8_t _6a;
      uint8_t _6b;
      uint8_t flg;
      uint8_t esa;
      uint8_t _6e;
      uint8_t C6;
      VoiceChannel voice7;
      uint8_t _7a;
      uint8_t _7b;
      uint8_t endx;
      uint8_t edl;
      uint8_t _7e;
      uint8_t C7;
    }reg;
    uint8_t regs[128];
  };
};

}
}
}
}