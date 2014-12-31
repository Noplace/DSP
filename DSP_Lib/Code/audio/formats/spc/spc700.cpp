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
#include "../../../dsp.h"
#include <functional>

namespace dsp {
namespace audio {
namespace formats {
namespace spc {

static const uint8_t ipl_rom[64] = {
0xCD,0xEF,
0xBD,
0xE8,0x00,
0xC6,
0x1D,
0xD0,0xFC,
0x8F,0xAA,0xF4,
0x8F,0xBB,0xF5,
0x78,0xCC,0xF4,
0xD0,0xFB,
0x2F,0x19,
0xEB,0xF4,
0xD0,0xFC,
0x7E,0xF4,
0xD0,0x0B,
0xE4,0xF5,
0xCB,0xF4,
0xD7,0x00,
0xFC,
0xD0,0xF3,
0xAB,0x01,
0x10,0xEF,
0x7E,0xF4,
0x10,0xEB,
0xBA,0xF6,
0xDA,0x00,
0xBA,0xF4,
0xC4,0xF4,
0xDD,
0x5D,
0xD0,0xDB,
0x1F,0x00,0x00,
0xC0,
0xFF
};

SPC700::SPC700() {
  cycles_ = 0;
  ram_ = nullptr;
  itable[0xE8] = &SPC700::MOV_A_imm;          itable[0xE6] = &SPC700::MOV_A_Xind;    
  itable[0xBF] = &SPC700::MOV_A_Xindautoinc;  itable[0xE4] = &SPC700::MOV_A_dp;
  itable[0xF4] = &SPC700::MOV_A_dpX;          itable[0xE5] = &SPC700::MOV_A_abs;     
  itable[0xF5] = &SPC700::MOV_A_absX;         itable[0xF6] = &SPC700::MOV_A_absY;
  itable[0xE7] = &SPC700::MOV_A_dpXind;       itable[0xF7] = &SPC700::MOV_A_dpYind; 
  itable[0xCD] = &SPC700::MOV_X_imm;          itable[0xF8] = &SPC700::MOV_X_dp;
  itable[0xF9] = &SPC700::MOV_X_dpY;          itable[0xE9] = &SPC700::MOV_X_abs;     
  itable[0x8D] = &SPC700::MOV_Y_imm;          itable[0xEB] = &SPC700::MOV_Y_dp;
  itable[0xFB] = &SPC700::MOV_Y_dpX;          itable[0xEC] = &SPC700::MOV_Y_abs;


  itable[0xC6] = &SPC700::MOV_Xind_A;
  itable[0xAF] = &SPC700::MOV_Xindautoinc_A;
  itable[0xC4] = &SPC700::MOV_dp_A;
  itable[0xD4] = &SPC700::MOV_dpX_A;
  itable[0xC5] = &SPC700::MOV_abs_A;
  itable[0xD5] = &SPC700::MOV_absX_A;
  itable[0xD6] = &SPC700::MOV_absY_A;
  itable[0xC7] = &SPC700::MOV_dpXind_A;
  itable[0xD7] = &SPC700::MOV_dpYind_A;
  itable[0xD8] = &SPC700::MOV_dp_X;
  itable[0xD9] = &SPC700::MOV_dpY_X;
  itable[0xC9] = &SPC700::MOV_abs_X;
  itable[0xCB] = &SPC700::MOV_dp_Y;
  itable[0xDB] = &SPC700::MOV_dpX_Y;
  itable[0xCC] = &SPC700::MOV_abs_Y;

  itable[0x7D] = &SPC700::MOV_A_X;            itable[0xDD] = &SPC700::MOV_A_Y;
  itable[0x5D] = &SPC700::MOV_X_A;            itable[0xFD] = &SPC700::MOV_Y_A;
  itable[0x9D] = &SPC700::MOV_X_SP;           itable[0xBD] = &SPC700::MOV_SP_X;
  itable[0xFA] = &SPC700::MOV_dp_dp;          itable[0x8F] = &SPC700::MOV_dp_imm;

  itable[0x88] = &SPC700::ADC_A_imm;          itable[0x86] = &SPC700::ADC_A_Xind;
  itable[0x84] = &SPC700::ADC_A_dp;           itable[0x94] = &SPC700::ADC_A_dpX; 
  itable[0x85] = &SPC700::ADC_A_abs;          itable[0x95] = &SPC700::ADC_A_absX;
  itable[0x96] = &SPC700::ADC_A_absY;         itable[0x87] = &SPC700::ADC_A_dpXind;
  itable[0x97] = &SPC700::ADC_A_dpYind;       itable[0x99] = &SPC700::ADC_Xind_Yind;
  itable[0x89] = &SPC700::ADC_dp_dp;          itable[0x98] = &SPC700::ADC_dp_imm;

  itable[0xA8] = &SPC700::SBC_A_imm;          itable[0xA6] = &SPC700::SBC_A_Xind;
  itable[0xA4] = &SPC700::SBC_A_dp;           itable[0xB4] = &SPC700::SBC_A_dpX; 
  itable[0xA5] = &SPC700::SBC_A_abs;          itable[0xB5] = &SPC700::SBC_A_absX;
  itable[0xB6] = &SPC700::SBC_A_absY;         itable[0xA7] = &SPC700::SBC_A_dpXind;
  itable[0xB7] = &SPC700::SBC_A_dpYind;       itable[0xB9] = &SPC700::SBC_Xind_Yind;
  itable[0xA9] = &SPC700::SBC_dp_dp;          itable[0xB8] = &SPC700::SBC_dp_imm;


  itable[0x68] = &SPC700::CMP_A_imm;   
  itable[0x66] = &SPC700::CMP_A_Xind;  
  itable[0x64] = &SPC700::CMP_A_dp;    
  itable[0x74] = &SPC700::CMP_A_dpX;   
  itable[0x65] = &SPC700::CMP_A_abs;   
  itable[0x75] = &SPC700::CMP_A_absX;  
  itable[0x76] = &SPC700::CMP_A_absY;  
  itable[0x67] = &SPC700::CMP_A_dpXind;
  itable[0x77] = &SPC700::CMP_A_dpYind;
  itable[0x79] = &SPC700::CMP_Xind_Yind;
  itable[0x69] = &SPC700::CMP_dp_dp;   
  itable[0x78] = &SPC700::CMP_dp_imm;  
  itable[0xC8] = &SPC700::CMP_X_imm;   
  itable[0x3E] = &SPC700::CMP_X_dp;    
  itable[0x1E] = &SPC700::CMP_X_abs;   
  itable[0xAD] = &SPC700::CMP_Y_imm;   
  itable[0x7E] = &SPC700::CMP_Y_dp;    
  itable[0x5E] = &SPC700::CMP_Y_abs;   


  itable[0x28] = &SPC700::AND_A_imm;   
  itable[0x26] = &SPC700::AND_A_Xind;  
  itable[0x24] = &SPC700::AND_A_dp;    
  itable[0x34] = &SPC700::AND_A_dpX;   
  itable[0x25] = &SPC700::AND_A_abs;   
  itable[0x35] = &SPC700::AND_A_absX;  
  itable[0x36] = &SPC700::AND_A_absY;  
  itable[0x27] = &SPC700::AND_A_dpXind;
  itable[0x37] = &SPC700::AND_A_dpYind;
  itable[0x39] = &SPC700::AND_Xind_Yind;
  itable[0x29] = &SPC700::AND_dp_dp;   
  itable[0x38] = &SPC700::AND_dp_imm;  

  itable[0x08] = &SPC700::OR_A_imm;   
  itable[0x06] = &SPC700::OR_A_Xind;  
  itable[0x04] = &SPC700::OR_A_dp;    
  itable[0x14] = &SPC700::OR_A_dpX;   
  itable[0x05] = &SPC700::OR_A_abs;   
  itable[0x15] = &SPC700::OR_A_absX;  
  itable[0x16] = &SPC700::OR_A_absY;  
  itable[0x07] = &SPC700::OR_A_dpXind;
  itable[0x17] = &SPC700::OR_A_dpYind;
  itable[0x19] = &SPC700::OR_Xind_Yind;
  itable[0x09] = &SPC700::OR_dp_dp;   
  itable[0x18] = &SPC700::OR_dp_imm;  

  itable[0x48] = &SPC700::EOR_A_imm;   
  itable[0x46] = &SPC700::EOR_A_Xind;  
  itable[0x44] = &SPC700::EOR_A_dp;    
  itable[0x54] = &SPC700::EOR_A_dpX;   
  itable[0x45] = &SPC700::EOR_A_abs;   
  itable[0x55] = &SPC700::EOR_A_absX;  
  itable[0x56] = &SPC700::EOR_A_absY;  
  itable[0x47] = &SPC700::EOR_A_dpXind;
  itable[0x57] = &SPC700::EOR_A_dpYind;
  itable[0x59] = &SPC700::EOR_Xind_Yind;
  itable[0x49] = &SPC700::EOR_dp_dp;   
  itable[0x58] = &SPC700::EOR_dp_imm;  

  itable[0xBC] = &SPC700::INC_A;
  itable[0xAB] = &SPC700::INC_dp;
  itable[0xBB] = &SPC700::INC_dpX;
  itable[0xAC] = &SPC700::INC_abs;
  itable[0x3D] = &SPC700::INC_X;
  itable[0xFC] = &SPC700::INC_Y;

  itable[0x9C] = &SPC700::DEC_A;
  itable[0x8B] = &SPC700::DEC_dp;
  itable[0x9B] = &SPC700::DEC_dpX;
  itable[0x8C] = &SPC700::DEC_abs;
  itable[0x1D] = &SPC700::DEC_X;
  itable[0xDC] = &SPC700::DEC_Y;

  itable[0x1C] = &SPC700::ASL_A;
  itable[0x0B] = &SPC700::ASL_dp;
  itable[0x1B] = &SPC700::ASL_dpX;
  itable[0x0C] = &SPC700::ASL_abs;

  itable[0x5C] = &SPC700::LSR_A;
  itable[0x4B] = &SPC700::LSR_dp;
  itable[0x5B] = &SPC700::LSR_dpX;
  itable[0x4C] = &SPC700::LSR_abs;

  itable[0x3C] = &SPC700::ROL_A;
  itable[0x2B] = &SPC700::ROL_dp;
  itable[0x3B] = &SPC700::ROL_dpX;
  itable[0x2C] = &SPC700::ROL_abs;

  itable[0x7C] = &SPC700::ROR_A;
  itable[0x6B] = &SPC700::ROR_dp;
  itable[0x7B] = &SPC700::ROR_dpX;
  itable[0x6C] = &SPC700::ROR_abs;

  itable[0x9F] = &SPC700::XCN;

  itable[0xBA] = &SPC700::MOVW_YA_dp;
  itable[0xDA] = &SPC700::MOVW_dp_YA;

  itable[0x3A] = &SPC700::INCW_dp;
  itable[0x1A] = &SPC700::DECW_dp;
  itable[0x7A] = &SPC700::ADDW_YA_dp;
  itable[0x9A] = &SPC700::SUBW_YA_dp;
  itable[0x5A] = &SPC700::CMPW_YA_dp;

  itable[0xCF] = &SPC700::MUL;
  itable[0x9E] = &SPC700::DIV;

  itable[0x2F] = &SPC700::BRA;
  itable[0xF0] = &SPC700::BEQ;
  itable[0xD0] = &SPC700::BNE;
  itable[0xB0] = &SPC700::BCS;
  itable[0x90] = &SPC700::BCC;
  itable[0x70] = &SPC700::BVS;
  itable[0x50] = &SPC700::BVC;
  itable[0x30] = &SPC700::BMI;
  itable[0x10] = &SPC700::BPL;


  itable[0x01] = &SPC700::TCALL<0xFFDE>;
  itable[0x11] = &SPC700::TCALL<0xFFDC>;
  itable[0x21] = &SPC700::TCALL<0xFFDA>;
  itable[0x31] = &SPC700::TCALL<0xFFD8>;
  itable[0x41] = &SPC700::TCALL<0xFFD6>;
  itable[0x51] = &SPC700::TCALL<0xFFD4>;
  itable[0x61] = &SPC700::TCALL<0xFFD2>;
  itable[0x71] = &SPC700::TCALL<0xFFD0>;
  itable[0x81] = &SPC700::TCALL<0xFFCE>;
  itable[0x91] = &SPC700::TCALL<0xFFCC>;
  itable[0xA1] = &SPC700::TCALL<0xFFCA>;
  itable[0xB1] = &SPC700::TCALL<0xFFC8>;
  itable[0xC1] = &SPC700::TCALL<0xFFC6>;
  itable[0xD1] = &SPC700::TCALL<0xFFC4>;
  itable[0xE1] = &SPC700::TCALL<0xFFC2>;
  itable[0xF1] = &SPC700::TCALL<0xFFC0>;

  

  itable[0xC0] = &SPC700::DI;

  itable[0x00] = &SPC700::NOP;
  itable[0xEF] = &SPC700::SLEEP;
  itable[0xFF] = &SPC700::STOP;
}

SPC700::~SPC700() {

}

void SPC700::Reset() {
  memset(&reg,0,sizeof(reg));
  cycles_ = 0;
  reg.PC = 0xFFC0;
  stop_ = false;
  sleep_ = false;

}

void SPC700::ExecuteInstruction() {
  if (stop_) return;
  auto opcode = ReadByte();
  (this->*(itable[opcode]))();
}

void SPC700::Execute(uint64_t cycles) {
  auto c0 = cycles_;
  while (cycles_-c0 <= cycles && stop_ == false) {
    ExecuteInstruction();
  }
}

uint8_t SPC700::ReadMem(uint16_t addr) {
  if (addr>=0xFFC0) {
    return ipl_rom[addr-0xFFC0];
  } else {
    
    switch (addr) {
      case 0xF2:
        
      break;
      case 0xF3:
        //ram_[addr] = dspread(ram_[0xF2]);
      break;
      case 0xF4:
      break;
      case 0xF5:
      break;
      case 0xF6:
      break;
      case 0xF7:
      break;
    }

    return ram_[addr];
  }
}

void SPC700::WriteMem(uint16_t addr, uint8_t value) {
  if (addr<0xFFC0) {

    ram_[addr] = value;
    switch (addr) {
      case 0xF2:
        
      break;
      case 0xF3:
        //dspwrite(ram_[0xF2],ram_[0xF3]);
      break;
    }
  }
}

void SPC700::MOV_A_imm() {
  reg.A = ReadImm();
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 2;
}

void SPC700::MOV_A_Xind() {
  reg.A = ReadMem(AddrXIndirect());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 3;
}

void SPC700::MOV_A_Xindautoinc() {
  reg.A = ReadMem(AddrXIndirectAutoInc());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 4;
}

void SPC700::MOV_A_dp() {
  reg.A = ReadMem(AddrDP());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 3;
}

void SPC700::MOV_A_dpX() {
  reg.A = ReadMem(AddrDPX());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 4;
}

void SPC700::MOV_A_abs() {
  reg.A = ReadMem(AddrAbs());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 4;
}

void SPC700::MOV_A_absX() {
  reg.A = ReadMem(AddrAbsX());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 5;
}

void SPC700::MOV_A_absY() {
  reg.A = ReadMem(AddrAbsY());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 5;
}

void SPC700::MOV_A_dpXind() {
  reg.A = ReadMem(AddrDPXIndirect());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 6;
}

void SPC700::MOV_A_dpYind() {
  reg.A = ReadMem(AddrDPYIndirect());
  UpdateFlags<FlagN|FlagZ>(reg.A);
  cycles_ += 6;
}

void SPC700::MOV_X_imm() {
  reg.X = ReadImm();
  UpdateFlags<FlagN|FlagZ>(reg.X);
  cycles_ += 2;
}

void SPC700::MOV_X_dp() {
  reg.X = ReadMem(AddrDP());
  UpdateFlags<FlagN|FlagZ>(reg.X);
  cycles_ += 3;
}

void SPC700::MOV_X_dpY()  {
  reg.X = ReadMem(AddrDPY());
  UpdateFlags<FlagN|FlagZ>(reg.X);
  cycles_ += 4;
}

void SPC700::MOV_X_abs()  {
  reg.X = ReadMem(AddrAbs());
  UpdateFlags<FlagN|FlagZ>(reg.X);
  cycles_ += 4;
}

void SPC700::MOV_Y_imm() {
  reg.Y = ReadImm();
  UpdateFlags<FlagN|FlagZ>(reg.Y);
  cycles_ += 2;
}

void SPC700::MOV_Y_dp() {
  reg.Y = ReadMem(AddrDP());
  UpdateFlags<FlagN|FlagZ>(reg.Y);
  cycles_ += 3;
}

void SPC700::MOV_Y_dpX()  {
  reg.Y = ReadMem(AddrDPX());
  UpdateFlags<FlagN|FlagZ>(reg.Y);
  cycles_ += 4;
}

void SPC700::MOV_Y_abs()  {
  reg.Y = ReadMem(AddrAbs());
  UpdateFlags<FlagN|FlagZ>(reg.Y);
  cycles_ += 4;
}


void SPC700::MOV_Xind_A() {
  WriteMem(reg.X,reg.A);
  cycles_ += 4;
}

void SPC700::MOV_Xindautoinc_A() {
  WriteMem(AddrXIndirectAutoInc(),reg.A);
  cycles_ += 4;
}

void SPC700::MOV_dp_A() {
  WriteMem(AddrDP(),reg.A);
  cycles_ += 4;
}


void SPC700::MOV_dpX_A() {
  WriteMem(AddrDPX(),reg.A);
  cycles_ += 5;
}


void SPC700::MOV_abs_A() {
  WriteMem(AddrAbs(),reg.A);
  cycles_ += 5;
}

void SPC700::MOV_absX_A() {
  WriteMem(AddrAbsX(),reg.A);
  cycles_ += 6;
}

void SPC700::MOV_absY_A() {
  WriteMem(AddrAbsY(),reg.A);
  cycles_ += 6;
}

void SPC700::MOV_dpXind_A() {
  WriteMem(AddrDPXIndirect(),reg.A);
  cycles_ += 7;
}

void SPC700::MOV_dpYind_A() {
  WriteMem(AddrDPYIndirect(),reg.A);
  cycles_ += 7;
}


void SPC700::MOV_dp_X() {
  WriteMem(AddrDP(),reg.X);
  cycles_ += 4;
}

void SPC700::MOV_dpY_X() {
  WriteMem(AddrDPY(),reg.X);
  cycles_ += 5;
}

void SPC700::MOV_abs_X() {
  WriteMem(AddrAbs(),reg.X);
  cycles_ += 5;
}

void SPC700::MOV_dp_Y() {
  WriteMem(AddrDP(),reg.Y);
  cycles_ += 4;
}

void SPC700::MOV_dpX_Y() {
  WriteMem(AddrDPY(),reg.Y);
  cycles_ += 5;
}

void SPC700::MOV_abs_Y() {
  WriteMem(AddrAbs(),reg.Y);
  cycles_ += 5;
}

void SPC700::MOV_A_X() {
  auto result = reg.A = reg.X;
  UpdateFlags<FlagN|FlagZ>(result);
  cycles_ += 2;
}

void SPC700::MOV_A_Y() {
  auto result = reg.A = reg.Y;
  UpdateFlags<FlagN|FlagZ>(result);
  cycles_ += 2;
}

void SPC700::MOV_X_A() {
  auto result = reg.X = reg.A;
  UpdateFlags<FlagN|FlagZ>(result);
  cycles_ += 2;
}

void SPC700::MOV_Y_A() {
  auto result = reg.Y = reg.A;
  UpdateFlags<FlagN|FlagZ>(result);
  cycles_ += 2;
}

void SPC700::MOV_X_SP() {
  auto result = reg.X = reg.SP;
  UpdateFlags<FlagN|FlagZ>(result);
  cycles_ += 2;
}

void SPC700::MOV_SP_X() {
  reg.SP = reg.X;
  cycles_ += 2;
}

void SPC700::MOV_dp_dp() {
  WriteMem(AddrDP(),ReadMem(AddrDP()));
  cycles_ += 5;
}

void SPC700::MOV_dp_imm() {
  WriteMem(AddrDP(),ReadImm());
  cycles_ += 5;
}



}
}
}
}