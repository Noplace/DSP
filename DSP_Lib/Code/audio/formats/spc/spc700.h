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

extern const uint8_t ipl_rom[64];

enum RegIndex {
  RegPCl = 0,
  RegPCh = 1,
  RegA = 2,
  RegY = 3,
  RegX = 4,
  RegSP = 5,
  RegPSW = 6
};

enum PSWFlags {
  FlagC = 1<<0,
  FlagZ = 1<<1,
  FlagH = 1<<3,
  FlagP = 1<<5,
  FlagV = 1<<6,
  FlagN = 1<<7
};

struct Registers {
  union {
    struct {
      uint16_t PC;
      union {
        struct {
          uint8_t A;
          uint8_t Y;
        };
        uint16_t YA;
      };
  
      uint8_t X;
      uint8_t SP;
      union {
        struct {
          uint8_t C:1;
          uint8_t Z:1;
          uint8_t _unused1:1;
          uint8_t H:1;
          uint8_t _unused2:1;
          uint8_t P:1;
          uint8_t V:1;
          uint8_t N:1;
        };
        uint8_t data;
      }PSW;  
    };
    uint8_t reg[7];
  };
};



class SPC700 {
 public:
  Registers reg;
  SPC700();
  ~SPC700();
  void Reset();
  void ExecuteInstruction();
  void Execute(uint64_t cycles);
  void set_mem(uint8_t* ram) { ram_ = ram; }
  uint8_t ReadMem(uint16_t addr);
  void WriteMem(uint16_t addr, uint8_t value);
  
  DSP* dsp;
 private:
  FILE* debug_fp;
  struct Timer {
    uint8_t internal;
    uint8_t limit;
    uint8_t counter:4;
  } timers[3];
  union {
    struct {
      uint8_t st0:1;
      uint8_t st1:1;
      uint8_t st2:1;
      uint8_t _unused1:1;
      uint8_t pc10:1;
      uint8_t pc23:1;
      uint8_t _unused2:1;
      uint8_t _unused3:1;
    };
    uint8_t data;
  } ctrl_reg;
  uint64_t cycles_,total_cycles_;
  uint8_t* ram_;
  bool stop_,sleep_;
  uint16_t effective_sp() { return 0x100 | reg.SP; }
  uint16_t dp() { return (reg.PSW.P<<8); }

  template<uint8_t flags>
  void UpdateFlags(uint16_t value,uint16_t m =0,uint16_t n = 0) {
    if (flags&FlagN)
      reg.PSW.N = (value & 0x80);
    if (flags&FlagV)
      reg.PSW.V = (m^value) & (n^value) & 0x80;
    if (flags&FlagH) {
      uint8_t r1 = (m&0xF) + (n&0xF);
      reg.PSW.H = r1>0xF?1:0;
    }
    if (flags&FlagZ)
      reg.PSW.Z = !value;
    if (flags&FlagC)
      reg.PSW.C = (value & 0x100);
      
  }

  void WriteWord(uint16_t addr, uint16_t value) {  
    WriteMem(addr,value&0xFF);
    WriteMem(addr+1,value>>8); 
  }

  uint8_t ReadByte(uint16_t addr) { return ReadMem(addr); }
  uint8_t ReadByte() { return ReadByte(reg.PC++); }
  uint16_t ReadWord(uint16_t addr) { return ReadByte(addr)+(ReadByte(addr+1)<<8); }
  uint16_t ReadWord() { uint16_t value = ReadWord(reg.PC); reg.PC+=2; return value; }
  uint8_t ReadImm() { return ReadByte(); }

  uint16_t AddrDP() { 
    return dp()+ReadByte();
  }

  uint16_t AddrDPX() { 
    uint16_t offset = ReadByte()+reg.X;
    return dp()+(offset&0xFF);
  }

  uint16_t AddrDPY() { 
    uint16_t offset = ReadByte()+reg.Y;
    return dp()+(offset&0xFF);
  }

  uint16_t AddrXIndirect() { 
    return dp()+reg.X;
  }

  uint16_t AddrXIndirectAutoInc() { 
    return dp()+reg.X++;
  }
  uint16_t AddrYIndirect() { 
    return dp()+reg.Y;
  }


  uint16_t AddrAbs() { 
    return ReadWord();
  }
  uint16_t AddrAbsX() { 
    return ReadWord()+reg.X;
  }
  uint16_t AddrAbsY() { 
    return ReadWord()+reg.Y;
  }

  uint16_t AddrDPXIndirect() { 
    auto addr0 = dp()+ReadByte()+reg.X;
    auto addr = ReadWord(addr0);
    return addr; 
  }

  uint16_t AddrDPYIndirect() { 
    auto addr0 = dp()+ReadByte();
    auto addr = ReadWord(addr0);
    return addr+reg.Y; 
  }

  struct DataPair {
    uint16_t addr;
    uint8_t value;
  };
  DataPair regA() { return {RegA,reg.A}; }
  DataPair regX() { return {RegX,reg.X}; }
  DataPair regY() { return {RegY,reg.Y}; }
  
//need to change below to make read write based on the proper functions

  DataPair SPC700::mem(uint16_t addr) {
    return {addr,ram_[addr]};
  }

  DataPair memImm() {
    return mem(reg.PC++);
  }
  DataPair memXIndirect() {
    return mem(AddrXIndirect());
  }

  DataPair memXIndirectAutoInc() {
    return mem(AddrXIndirectAutoInc());
  }

  DataPair memYIndirect() {
    return mem(AddrYIndirect());
  }

  DataPair memDP() {
    return mem(AddrDP());
  }

  DataPair memDPX() {
    return mem(AddrDPX());
  }

  DataPair memDPY() {
    return mem(AddrDPY());
  }

  DataPair memAbs() {
    return mem(AddrAbs());
  }

  DataPair memAbsX() {
    return mem(AddrAbsX());
  }

  DataPair memAbsY() {
    return mem(AddrAbsY());
  }

  DataPair memDPXIndirect() {
    return mem(AddrDPXIndirect());
  }

  DataPair memDPYYndirect() {
    return mem(AddrDPYIndirect());
  }

  void WriteReg(uint16_t addr, uint8_t value) {
    reg.reg[addr] = value;
  }

  typedef void (SPC700::*Instruction)();
  Instruction itable[0xFF];

  /*template<RegIndex regindex,class ReadOP,class UpdateOP, uint8_t cycles>
  void MOV() {
    ReadOP readOp;
    UpdateOP update;
    reg.reg[regindex] = readOp();
    update(reg.reg[regindex]);
    cycles_ += cycles;
  }*/

  void MOV_A_imm();
  void MOV_A_Xind();
  void MOV_A_Xindautoinc();
  void MOV_A_dp();
  void MOV_A_dpX();
  void MOV_A_abs();
  void MOV_A_absX();
  void MOV_A_absY();
  void MOV_A_dpXind();
  void MOV_A_dpYind();
  void MOV_X_imm();
  void MOV_X_dp();
  void MOV_X_dpY();
  void MOV_X_abs();
  void MOV_Y_imm();
  void MOV_Y_dp();
  void MOV_Y_dpX();
  void MOV_Y_abs();

  void MOV_Xind_A();
  void MOV_Xindautoinc_A();
  void MOV_dp_A();
  void MOV_dpX_A();
  void MOV_abs_A();
  void MOV_absX_A();
  void MOV_absY_A();
  void MOV_dpXind_A();
  void MOV_dpYind_A();
  void MOV_dp_X();
  void MOV_dpY_X();
  void MOV_abs_X();
  void MOV_dp_Y();
  void MOV_dpX_Y();
  void MOV_abs_Y();
  
  void MOV_A_X();
  void MOV_A_Y();
  void MOV_X_A();
  void MOV_Y_A();
  void MOV_X_SP();
  void MOV_SP_X();
  void MOV_dp_dp();
  void MOV_dp_imm();
  
  

  //typedef void (*SPC700::UpdateFunc)(uint16_t,uint8_t);
  template<uint8_t cycles,class OP1,class OP2, class UpdateFunc>
  void ADC(OP1 op1, OP2 op2,UpdateFunc updateFunc) {
    DataPair m = (this->*op1)();
    DataPair n =  (this->*op2)();
    uint16_t result = m.value + n.value + reg.PSW.C;
    UpdateFlags<FlagN|FlagV|FlagH|FlagZ|FlagC>(result,m.value,n.value);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void ADC_A_imm() { return ADC<2>(&SPC700::regA,&SPC700::memImm,&SPC700::WriteReg); }
  void ADC_A_Xind() { return ADC<3>(&SPC700::regA,&SPC700::memXIndirect,&SPC700::WriteReg); }
  void ADC_A_dp() { return ADC<3>(&SPC700::regA,&SPC700::memDP,&SPC700::WriteReg); }
  void ADC_A_dpX() { return ADC<4>(&SPC700::regA,&SPC700::memDPX,&SPC700::WriteReg); }
  void ADC_A_abs() { return ADC<4>(&SPC700::regA,&SPC700::memAbs,&SPC700::WriteReg); }
  void ADC_A_absX() { return ADC<5>(&SPC700::regA,&SPC700::memAbsX,&SPC700::WriteReg); }
  void ADC_A_absY() { return ADC<5>(&SPC700::regA,&SPC700::memAbsY,&SPC700::WriteReg); }
  void ADC_A_dpXind()  { return ADC<6>(&SPC700::regA,&SPC700::memDPXIndirect,&SPC700::WriteReg); }
  void ADC_A_dpYind(){ return ADC<6>(&SPC700::regA,&SPC700::memDPYYndirect,&SPC700::WriteReg); }
  void ADC_Xind_Yind() { return ADC<5>(&SPC700::memXIndirect,&SPC700::memYIndirect,&SPC700::WriteMem); }
  void ADC_dp_dp() { return ADC<6>(&SPC700::memDP,&SPC700::memDP,&SPC700::WriteMem); }
  void ADC_dp_imm() { return ADC<5>(&SPC700::memDP,&SPC700::memImm,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class OP2,class UpdateFunc>
  void SBC(OP1 op1, OP2 op2,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    auto n =  (this->*op2)();
    uint16_t result = m.value - n.value + !reg.PSW.C;
    UpdateFlags<FlagN|FlagV|FlagH|FlagZ|FlagC>(result,m.value,n.value);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void SBC_A_imm() { return SBC<2>(&SPC700::regA,&SPC700::memImm,&SPC700::WriteReg); }
  void SBC_A_Xind() { return SBC<3>(&SPC700::regA,&SPC700::memXIndirect,&SPC700::WriteReg); }
  void SBC_A_dp() { return SBC<3>(&SPC700::regA,&SPC700::memDP,&SPC700::WriteReg); }
  void SBC_A_dpX() { return SBC<4>(&SPC700::regA,&SPC700::memDPX,&SPC700::WriteReg); }
  void SBC_A_abs() { return SBC<4>(&SPC700::regA,&SPC700::memAbs,&SPC700::WriteReg); }
  void SBC_A_absX() { return SBC<5>(&SPC700::regA,&SPC700::memAbsX,&SPC700::WriteReg); }
  void SBC_A_absY() { return SBC<5>(&SPC700::regA,&SPC700::memAbsY,&SPC700::WriteReg); }
  void SBC_A_dpXind()  { return SBC<6>(&SPC700::regA,&SPC700::memDPXIndirect,&SPC700::WriteReg); }
  void SBC_A_dpYind(){ return SBC<6>(&SPC700::regA,&SPC700::memDPYYndirect,&SPC700::WriteReg); }
  void SBC_Xind_Yind() { return SBC<5>(&SPC700::memXIndirect,&SPC700::memYIndirect,&SPC700::WriteMem); }
  void SBC_dp_dp() { return SBC<6>(&SPC700::memDP,&SPC700::memDP,&SPC700::WriteMem); }
  void SBC_dp_imm() { return SBC<5>(&SPC700::memDP,&SPC700::memImm,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class OP2>
  void CMP(OP1 op1, OP2 op2) {
    auto m = (this->*op1)();
    auto n =  (this->*op2)();
    uint16_t result = m.value - n.value;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m.value,n.value);
    cycles_ += cycles;
  }

  void CMP_A_imm()     { return CMP<2>(&SPC700::regA,&SPC700::memImm); }
  void CMP_A_Xind()    { return CMP<3>(&SPC700::regA,&SPC700::memXIndirect); }
  void CMP_A_dp()      { return CMP<3>(&SPC700::regA,&SPC700::memDP); }
  void CMP_A_dpX()     { return CMP<4>(&SPC700::regA,&SPC700::memDPX); }
  void CMP_A_abs()     { return CMP<4>(&SPC700::regA,&SPC700::memAbs); }
  void CMP_A_absX()    { return CMP<5>(&SPC700::regA,&SPC700::memAbsX); }
  void CMP_A_absY()    { return CMP<5>(&SPC700::regA,&SPC700::memAbsY); }
  void CMP_A_dpXind()  { return CMP<6>(&SPC700::regA,&SPC700::memDPXIndirect); }
  void CMP_A_dpYind()  { return CMP<6>(&SPC700::regA,&SPC700::memDPYYndirect); }
  void CMP_Xind_Yind() { return CMP<5>(&SPC700::memXIndirect,&SPC700::memYIndirect); }
  void CMP_dp_dp()     { return CMP<6>(&SPC700::memDP,&SPC700::memDP); }
  void CMP_dp_imm()    { return CMP<5>(&SPC700::memDP,&SPC700::memImm); }
  void CMP_X_imm()     { return CMP<2>(&SPC700::regX,&SPC700::memImm); }
  void CMP_X_dp()      { return CMP<3>(&SPC700::regX,&SPC700::memDP); }
  void CMP_X_abs()     { return CMP<4>(&SPC700::regX,&SPC700::memAbs); }
  void CMP_Y_imm()     { return CMP<2>(&SPC700::regY,&SPC700::memImm); }
  void CMP_Y_dp()      { return CMP<3>(&SPC700::regY,&SPC700::memDP); }
  void CMP_Y_abs()     { return CMP<4>(&SPC700::regY,&SPC700::memAbs); }

  template<uint8_t cycles,class OP1,class OP2,class UpdateFunc>
  void AND(OP1 op1, OP2 op2,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    auto n =  (this->*op2)();
    uint16_t result = m.value & n.value;
    UpdateFlags<FlagN|FlagZ>(result,m.value,n.value);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void AND_A_imm()     { return AND<2>(&SPC700::regA,&SPC700::memImm,&SPC700::WriteReg); }
  void AND_A_Xind()    { return AND<3>(&SPC700::regA,&SPC700::memXIndirect,&SPC700::WriteReg); }
  void AND_A_dp()      { return AND<3>(&SPC700::regA,&SPC700::memDP,&SPC700::WriteReg); }
  void AND_A_dpX()     { return AND<4>(&SPC700::regA,&SPC700::memDPX,&SPC700::WriteReg); }
  void AND_A_abs()     { return AND<4>(&SPC700::regA,&SPC700::memAbs,&SPC700::WriteReg); }
  void AND_A_absX()    { return AND<5>(&SPC700::regA,&SPC700::memAbsX,&SPC700::WriteReg); }
  void AND_A_absY()    { return AND<5>(&SPC700::regA,&SPC700::memAbsY,&SPC700::WriteReg); }
  void AND_A_dpXind()  { return AND<6>(&SPC700::regA,&SPC700::memDPXIndirect,&SPC700::WriteReg); }
  void AND_A_dpYind()  { return AND<6>(&SPC700::regA,&SPC700::memDPYYndirect,&SPC700::WriteReg); }
  void AND_Xind_Yind() { return AND<5>(&SPC700::memXIndirect,&SPC700::memYIndirect,&SPC700::WriteMem); }
  void AND_dp_dp()     { return AND<6>(&SPC700::memDP,&SPC700::memDP,&SPC700::WriteMem); }
  void AND_dp_imm()    { return AND<5>(&SPC700::memDP,&SPC700::memImm,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class OP2,class UpdateFunc>
  void OR(OP1 op1, OP2 op2,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    auto n =  (this->*op2)();
    uint16_t result = m.value | n.value;
    UpdateFlags<FlagN|FlagZ>(result,m.value,n.value);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void OR_A_imm()     { return OR<2>(&SPC700::regA,&SPC700::memImm,&SPC700::WriteReg); }
  void OR_A_Xind()    { return OR<3>(&SPC700::regA,&SPC700::memXIndirect,&SPC700::WriteReg); }
  void OR_A_dp()      { return OR<3>(&SPC700::regA,&SPC700::memDP,&SPC700::WriteReg); }
  void OR_A_dpX()     { return OR<4>(&SPC700::regA,&SPC700::memDPX,&SPC700::WriteReg); }
  void OR_A_abs()     { return OR<4>(&SPC700::regA,&SPC700::memAbs,&SPC700::WriteReg); }
  void OR_A_absX()    { return OR<5>(&SPC700::regA,&SPC700::memAbsX,&SPC700::WriteReg); }
  void OR_A_absY()    { return OR<5>(&SPC700::regA,&SPC700::memAbsY,&SPC700::WriteReg); }
  void OR_A_dpXind()  { return OR<6>(&SPC700::regA,&SPC700::memDPXIndirect,&SPC700::WriteReg); }
  void OR_A_dpYind()  { return OR<6>(&SPC700::regA,&SPC700::memDPYYndirect,&SPC700::WriteReg); }
  void OR_Xind_Yind() { return OR<5>(&SPC700::memXIndirect,&SPC700::memYIndirect,&SPC700::WriteMem); }
  void OR_dp_dp()     { return OR<6>(&SPC700::memDP,&SPC700::memDP,&SPC700::WriteMem); }
  void OR_dp_imm()    { return OR<5>(&SPC700::memDP,&SPC700::memImm,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class OP2,class UpdateFunc>
  void EOR(OP1 op1, OP2 op2,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    auto n =  (this->*op2)();
    uint16_t result = m.value ^ n.value;
    UpdateFlags<FlagN|FlagZ>(result,m.value,n.value);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void EOR_A_imm()     { return EOR<2>(&SPC700::regA,&SPC700::memImm,&SPC700::WriteReg); }
  void EOR_A_Xind()    { return EOR<3>(&SPC700::regA,&SPC700::memXIndirect,&SPC700::WriteReg); }
  void EOR_A_dp()      { return EOR<3>(&SPC700::regA,&SPC700::memDP,&SPC700::WriteReg); }
  void EOR_A_dpX()     { return EOR<4>(&SPC700::regA,&SPC700::memDPX,&SPC700::WriteReg); }
  void EOR_A_abs()     { return EOR<4>(&SPC700::regA,&SPC700::memAbs,&SPC700::WriteReg); }
  void EOR_A_absX()    { return EOR<5>(&SPC700::regA,&SPC700::memAbsX,&SPC700::WriteReg); }
  void EOR_A_absY()    { return EOR<5>(&SPC700::regA,&SPC700::memAbsY,&SPC700::WriteReg); }
  void EOR_A_dpXind()  { return EOR<6>(&SPC700::regA,&SPC700::memDPXIndirect,&SPC700::WriteReg); }
  void EOR_A_dpYind()  { return EOR<6>(&SPC700::regA,&SPC700::memDPYYndirect,&SPC700::WriteReg); }
  void EOR_Xind_Yind() { return EOR<5>(&SPC700::memXIndirect,&SPC700::memYIndirect,&SPC700::WriteMem); }
  void EOR_dp_dp()     { return EOR<6>(&SPC700::memDP,&SPC700::memDP,&SPC700::WriteMem); }
  void EOR_dp_imm()    { return EOR<5>(&SPC700::memDP,&SPC700::memImm,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class UpdateFunc>
  void INC(OP1 op1,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    uint16_t result = ++m.value;
    UpdateFlags<FlagN|FlagZ>(result,m.value,0);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void INC_A()     { return INC<2>(&SPC700::regA,&SPC700::WriteReg); }
  void INC_dp()    { return INC<4>(&SPC700::memDP,&SPC700::WriteMem); }
  void INC_dpX()   { return INC<5>(&SPC700::memDPX,&SPC700::WriteMem); }
  void INC_abs()   { return INC<5>(&SPC700::memAbs,&SPC700::WriteMem); }
  void INC_X()     { return INC<2>(&SPC700::regX,&SPC700::WriteReg); }
  void INC_Y()     { return INC<2>(&SPC700::regY,&SPC700::WriteReg); }

  template<uint8_t cycles,class OP1,class UpdateFunc>
  void DEC(OP1 op1,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    uint16_t result = --m.value;
    UpdateFlags<FlagN|FlagZ>(result,m.value,0);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void DEC_A()     { return DEC<2>(&SPC700::regA,&SPC700::WriteReg); }
  void DEC_dp()    { return DEC<4>(&SPC700::memDP,&SPC700::WriteMem); }
  void DEC_dpX()   { return DEC<5>(&SPC700::memDPX,&SPC700::WriteMem); }
  void DEC_abs()   { return DEC<5>(&SPC700::memAbs,&SPC700::WriteMem); }
  void DEC_X()     { return DEC<2>(&SPC700::regX,&SPC700::WriteReg); }
  void DEC_Y()     { return DEC<2>(&SPC700::regY,&SPC700::WriteReg); }

  template<uint8_t cycles,class OP1,class UpdateFunc>
  void ASL(OP1 op1,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    reg.PSW.C = m.value & 0x80;
    uint16_t result = m.value << 1;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m.value,0);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void ASL_A()     { return ASL<2>(&SPC700::regA,&SPC700::WriteReg); }
  void ASL_dp()    { return ASL<4>(&SPC700::memDP,&SPC700::WriteMem); }
  void ASL_dpX()   { return ASL<5>(&SPC700::memDPX,&SPC700::WriteMem); }
  void ASL_abs()   { return ASL<5>(&SPC700::memAbs,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class UpdateFunc>
  void LSR(OP1 op1,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    reg.PSW.C = m.value & 1;
    uint16_t result = m.value >> 1;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m.value,0);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void LSR_A()     { return LSR<2>(&SPC700::regA,&SPC700::WriteReg); }
  void LSR_dp()    { return LSR<4>(&SPC700::memDP,&SPC700::WriteMem); }
  void LSR_dpX()   { return LSR<5>(&SPC700::memDPX,&SPC700::WriteMem); }
  void LSR_abs()   { return LSR<5>(&SPC700::memAbs,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class UpdateFunc>
  void ROL(OP1 op1,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    uint8_t oldC = reg.PSW.C;
    reg.PSW.C = m.value & 0x80;
    uint16_t result = (m.value << 1) | oldC;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m.value,0);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void ROL_A()     { return ROL<2>(&SPC700::regA,&SPC700::WriteReg); }
  void ROL_dp()    { return ROL<4>(&SPC700::memDP,&SPC700::WriteMem); }
  void ROL_dpX()   { return ROL<5>(&SPC700::memDPX,&SPC700::WriteMem); }
  void ROL_abs()   { return ROL<5>(&SPC700::memAbs,&SPC700::WriteMem); }

  template<uint8_t cycles,class OP1,class UpdateFunc>
  void ROR(OP1 op1,UpdateFunc updateFunc) {
    auto m = (this->*op1)();
    uint8_t oldC = reg.PSW.C;
    reg.PSW.C = m.value & 0x1;
    uint16_t result = (m.value >> 1) | (oldC<<7);
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m.value,0);
    (this->*(updateFunc))(m.addr,result & 0xFF);
    cycles_ += cycles;
  }

  void ROR_A()     { return ROR<2>(&SPC700::regA,&SPC700::WriteReg); }
  void ROR_dp()    { return ROR<4>(&SPC700::memDP,&SPC700::WriteMem); }
  void ROR_dpX()   { return ROR<5>(&SPC700::memDPX,&SPC700::WriteMem); }
  void ROR_abs()   { return ROR<5>(&SPC700::memAbs,&SPC700::WriteMem); }

  void XCN() {
    uint8_t n1 = (reg.A & 0xF) << 4;
    uint8_t n2 = (reg.A & 0xF0) >> 4;
    reg.A = n1 | n2;
  }


  void MOVW_YA_dp() { 
    auto addr = AddrDP();
    auto result = ReadWord(addr);
    reg.YA = result;
    UpdateFlags<FlagN|FlagZ>(result,0,0);
    cycles_ += 5;
  }
  void MOVW_dp_YA() { 
    auto addr = AddrDP();
    WriteWord(addr,reg.YA);
    cycles_ += 5;
  }

  void INCW_dp() { 
    auto addr = AddrDP();
    auto result = ReadWord(addr);
    ++result;
    WriteWord(addr,result);
    UpdateFlags<FlagN|FlagZ>(result,0,0);
    cycles_ += 6;
  }

  void DECW_dp() { 
    auto addr = AddrDP();
    auto result = ReadWord(addr);
    --result;
    WriteWord(addr,result);
    UpdateFlags<FlagN|FlagZ>(result,0,0);
    cycles_ += 6;
  }

  void ADDW_YA_dp() { 
    uint16_t addr = AddrDP();
    uint16_t n = ReadWord(addr);
    uint16_t m = reg.YA;
    uint16_t result = m + n;
    UpdateFlags<FlagN|FlagV||FlagH|FlagZ|FlagC>(result,m,n);
    reg.YA = result;
    cycles_ += 5;
  }

  void SUBW_YA_dp() { 
    uint16_t addr = AddrDP();
    uint16_t n = ReadWord(addr);
    uint16_t m = reg.YA;
    uint16_t result = m - n;
    UpdateFlags<FlagN|FlagV||FlagH|FlagZ|FlagC>(result,m,n);
    reg.YA = result;
    cycles_ += 5;
  }

  void CMPW_YA_dp() { 
    uint16_t addr = AddrDP();
    uint16_t n = ReadWord(addr);
    uint16_t m = reg.YA;
    uint16_t result = m - n;
    UpdateFlags<FlagN||FlagZ|FlagC>(result,m,n);
    cycles_ += 5;
  }

  void MUL() { 
    uint16_t result = reg.Y * reg.A;
    UpdateFlags<FlagN||FlagZ>(result,0,0);
    reg.YA = result;
    cycles_ += 9;
  }

  void DIV() { 
    uint8_t tmp1 = (reg.YA % reg.X) & 0xFF;
    uint8_t tmp2 = (reg.YA / reg.X) & 0xFF;
    reg.Y = tmp1;
    reg.A = tmp2;
    UpdateFlags<FlagN||FlagZ>(reg.YA,tmp1,tmp2);
    cycles_ += 12;
  }


  template<typename Cond>
  void Branch(Cond cond) {
    int8_t rel = (int8_t)ReadByte();
    cycles_ += 2;
    if (cond()) {
      reg.PC += rel;
      cycles_ += 2;
    }
  }

  void BRA() {    Branch([this](){ return 1 == 1; });  }
  void BEQ() {    Branch([this](){ return reg.PSW.Z == 1; });  }
  void BNE() {    Branch([this](){ return reg.PSW.Z == 0; });  }
  void BCS() {    Branch([this](){ return reg.PSW.C == 1; });  }
  void BCC() {    Branch([this](){ return reg.PSW.C == 0; });  }
  void BVS() {    Branch([this](){ return reg.PSW.V == 1; });  }
  void BVC() {    Branch([this](){ return reg.PSW.V == 0; });  }
  void BMI() {    Branch([this](){ return reg.PSW.N == 1; });  }
  void BPL() {    Branch([this](){ return reg.PSW.N == 0; });  }

  template<uint8_t bit>
  void BBS() {
    auto mem = memDP();cycles_ += 3;
    Branch([this,mem](){ return mem.value&(1<<bit); });
  }

  template<uint8_t bit>
  void BBC() {
    auto mem = memDP();cycles_ += 3;
    Branch([this,mem](){ return (mem.value&(1<<bit))==0; });
  }


  void CBNE_dp() {
    auto mem = memDP();cycles_ += 3;
    Branch([this,mem](){ return reg.A != mem.value; });
  }


  void CBNE_dpX() {
    auto mem = memDPX();cycles_ += 4;
    Branch([this,mem](){ return reg.A != mem.value; });
  }
  
  void JMP_abs() {
    uint16_t addr = AddrAbs();
    reg.PC = addr;
    cycles_ += 3;
  }


  void JMP_absX() {
    uint16_t addr = AddrAbsX();
    reg.PC = addr;
    cycles_ += 6;
  }

  void CALL() {
    uint16_t addr = ReadWord();
    WriteMem(0x100|(reg.SP--),reg.PC>>8);
    WriteMem(0x100|(reg.SP--),reg.PC&0xFF);
    reg.PC = addr;
    cycles_ += 8;
  }


  void RET() {
    uint8_t PCL = ReadMem((++reg.SP)|0x100);
    uint8_t PCH = ReadMem((++reg.SP)|0x100);
    reg.PC = (PCH<<8)|PCL;
    cycles_ += 5;
  }

  template<uint16_t addr>
  void TCALL() { reg.PC = addr; cycles_+=8; }

  void PUSH_A() {
    WriteMem(0x100|(reg.SP--),reg.A);
    cycles_ += 4;
  }

  void PUSH_X() {
    WriteMem(0x100|(reg.SP--),reg.X);
    cycles_ += 4;
  }

  void PUSH_Y() {
    WriteMem(0x100|(reg.SP--),reg.Y);
    cycles_ += 4;
  }

  void PUSH_PSW() {
    WriteMem(0x100|(reg.SP--),reg.PSW.data);
    cycles_ += 4;
  }

  void POP_A() {
    reg.A = ReadMem((++reg.SP)|0x100);
    cycles_ += 4;
  }

  void POP_X() {
    reg.X = ReadMem((++reg.SP)|0x100);
    cycles_ += 4;
  }

  void POP_Y() {
    reg.Y = ReadMem((++reg.SP)|0x100);
    cycles_ += 4;
  }

  void POP_PSW() {
    reg.PSW.data = ReadMem((++reg.SP)|0x100);
    cycles_ += 4;
  }





  template<uint8_t bit>
  void SET1() {
    auto m = memDP();
    m.value |= 1<<bit;
    WriteMem(m.addr,m.value);
    cycles_ += 4;
  }

  template<uint8_t bit>
  void CLR1() {
    auto m = memDP();
    m.value &= ~(1<<bit);
    WriteMem(m.addr,m.value);
    cycles_ += 4;
  }


  void TSET1() {
    auto m = memAbs();
    m.value |= reg.A;
    WriteMem(m.addr,m.value);
    UpdateFlags<FlagN|FlagZ>(reg.A-m.value);
    cycles_ += 6;
  }


  void TCLR1() {
    auto m = memAbs();
    m.value &= ~reg.A;
    WriteMem(m.addr,m.value);
    UpdateFlags<FlagN|FlagZ>(reg.A-m.value);
    cycles_ += 6;
  }

  void CLRC() { reg.PSW.C = 0; cycles_+=2; }
  void SETC() { reg.PSW.C = 1; cycles_+=2; }
  void NOTC() { reg.PSW.C = !reg.PSW.C; cycles_+=3; }
  void CLRV() { reg.PSW.V = 0; cycles_+=2; }
  void CLRP() { reg.PSW.P = 0; cycles_+=2; }
  void SETP() { reg.PSW.P = 1; cycles_+=2; }
  void EI() { reg.PSW._unused1 = 1; cycles_+=2;/* no interrupts anyway */ }
  void DI() { reg.PSW._unused1 = 0; cycles_+=2;/* no interrupts anyway */ }

  void NOP() { }
  void SLEEP() { sleep_ = true; }
  void STOP() { stop_ = true; }
};

}
}
}
}