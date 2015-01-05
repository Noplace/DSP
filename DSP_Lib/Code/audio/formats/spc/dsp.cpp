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

#include "../../../dsp.h"

namespace dsp {
namespace audio {
namespace formats {
namespace spc {


void BRRDecode(uint8_t* block) {
  auto header = block[0];
  uint8_t range = header>>4;
  uint8_t findex = (header>>2)&0x3; 
  uint8_t loop = (header>>1)&0x1;
  const float filters[4][2] = { {0.0f,0.0f},{0.9375f,0},{1.90625f  ,-0.9375f},{1.796875f ,-0.8125f} };

  uint16_t samples[16];
  static uint16_t y=0,z=0;
  for(int i=0;i<8;++i) {
    samples[(i<<1)] = block[1+i]>>4;
    samples[(i<<1)+1] = block[1+i]&0x0F;
  }

  auto a = filters[findex][0];
  auto b = filters[findex][1];
  for(int i=0;i<16;++i) {
    samples[i] <<= range;
    samples[i] = (uint16_t)(samples[i] + a*y + b*z);
    z = y;
    y = samples[i];
  }
}

void DSP::Reset() {
  memset(regs,0,sizeof(reg));
}

uint8_t DSP::Read(uint8_t addr) {
  return regs[addr];
}

void DSP::Write(uint8_t addr, uint8_t value) {
  regs[addr] = value;
}

short DSP::Tick() {

}

}
}
}
}