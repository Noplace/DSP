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
 private:
  uint64_t cycles_;
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
  uint16_t ReadWord() { return ReadWord(reg.PC+=2); }
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

  typedef void (SPC700::*Instruction)();
  Instruction itable[0xFF];

  template<RegIndex regindex,class ReadOP,class UpdateOP, uint8_t cycles>
  void MOV() {
    ReadOP readOp;
    UpdateOP update;
    reg.reg[regindex] = readOp();
    update(reg.reg[regindex]);
    cycles_ += cycles;
  }

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

  uint8_t& regA() { return reg.A; }
  uint8_t& regX() { return reg.X; }
  uint8_t& regY() { return reg.Y; }
  

  uint8_t& SPC700::mem(uint16_t addr) {
    return ram_[addr];
  }

  uint8_t& memXIndirect() {
    return mem(AddrXIndirect());
  }

  uint8_t& memXIndirectAutoInc() {
    return mem(AddrXIndirectAutoInc());
  }

  uint8_t& memYIndirect() {
    return mem(AddrYIndirect());
  }

  uint8_t& memDP() {
    return mem(AddrDP());
  }

  uint8_t& memDPX() {
    return mem(AddrDPX());
  }

  uint8_t& memDPY() {
    return mem(AddrDPY());
  }

  uint8_t& memAbs() {
    return mem(AddrAbs());
  }

  uint8_t& memAbsX() {
    return mem(AddrAbsX());
  }

  uint8_t& memAbsY() {
    return mem(AddrAbsY());
  }

  uint8_t& memDPXIndirect() {
    return mem(AddrDPXIndirect());
  }

  uint8_t& memDPYYndirect() {
    return mem(AddrDPYIndirect());
  }

  template<uint8_t cycles,class OP1,class OP2>
  void ADC(OP1 op1, OP2 op2) {
    uint8_t& m = (this->*op1)();
    uint8_t n =  (this->*op2)();
    uint16_t result = m + n + reg.PSW.C;
    UpdateFlags<FlagN|FlagV|FlagH|FlagZ|FlagC>(result,m,n);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void ADC_A_imm() { return ADC<2>(&SPC700::regA,&SPC700::ReadImm); }
  void ADC_A_Xind() { return ADC<3>(&SPC700::regA,&SPC700::memXIndirect); }
  void ADC_A_dp() { return ADC<3>(&SPC700::regA,&SPC700::memDP); }
  void ADC_A_dpX() { return ADC<4>(&SPC700::regA,&SPC700::memDPX); }
  void ADC_A_abs() { return ADC<4>(&SPC700::regA,&SPC700::memAbs); }
  void ADC_A_absX() { return ADC<5>(&SPC700::regA,&SPC700::memAbsX); }
  void ADC_A_absY() { return ADC<5>(&SPC700::regA,&SPC700::memAbsY); }
  void ADC_A_dpXind()  { return ADC<6>(&SPC700::regA,&SPC700::memDPXIndirect); }
  void ADC_A_dpYind(){ return ADC<6>(&SPC700::regA,&SPC700::memDPYYndirect); }
  void ADC_Xind_Yind() { return ADC<5>(&SPC700::memXIndirect,&SPC700::memYIndirect); }
  void ADC_dp_dp() { return ADC<6>(&SPC700::memDP,&SPC700::memDP); }
  void ADC_dp_imm() { return ADC<5>(&SPC700::memDP,&SPC700::ReadImm); }

  template<uint8_t cycles,class OP1,class OP2>
  void SBC(OP1 op1, OP2 op2) {
    uint8_t& m = (this->*op1)();
    uint8_t n =  (this->*op2)();
    uint16_t result = m - n + !reg.PSW.C;
    UpdateFlags<FlagN|FlagV|FlagH|FlagZ|FlagC>(result,m,n);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void SBC_A_imm() { return SBC<2>(&SPC700::regA,&SPC700::ReadImm); }
  void SBC_A_Xind() { return SBC<3>(&SPC700::regA,&SPC700::memXIndirect); }
  void SBC_A_dp() { return SBC<3>(&SPC700::regA,&SPC700::memDP); }
  void SBC_A_dpX() { return SBC<4>(&SPC700::regA,&SPC700::memDPX); }
  void SBC_A_abs() { return SBC<4>(&SPC700::regA,&SPC700::memAbs); }
  void SBC_A_absX() { return SBC<5>(&SPC700::regA,&SPC700::memAbsX); }
  void SBC_A_absY() { return SBC<5>(&SPC700::regA,&SPC700::memAbsY); }
  void SBC_A_dpXind()  { return SBC<6>(&SPC700::regA,&SPC700::memDPXIndirect); }
  void SBC_A_dpYind(){ return SBC<6>(&SPC700::regA,&SPC700::memDPYYndirect); }
  void SBC_Xind_Yind() { return SBC<5>(&SPC700::memXIndirect,&SPC700::memYIndirect); }
  void SBC_dp_dp() { return SBC<6>(&SPC700::memDP,&SPC700::memDP); }
  void SBC_dp_imm() { return SBC<5>(&SPC700::memDP,&SPC700::ReadImm); }

  template<uint8_t cycles,class OP1,class OP2>
  void CMP(OP1 op1, OP2 op2) {
    uint8_t m = (this->*op1)();
    uint8_t n =  (this->*op2)();
    uint16_t result = m - n;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m,n);
    cycles_ += cycles;
  }

  void CMP_A_imm()     { return CMP<2>(&SPC700::regA,&SPC700::ReadImm); }
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
  void CMP_dp_imm()    { return CMP<5>(&SPC700::memDP,&SPC700::ReadImm); }
  void CMP_X_imm()     { return CMP<2>(&SPC700::regX,&SPC700::ReadImm); }
  void CMP_X_dp()      { return CMP<3>(&SPC700::regX,&SPC700::memDP); }
  void CMP_X_abs()     { return CMP<4>(&SPC700::regX,&SPC700::memAbs); }
  void CMP_Y_imm()     { return CMP<2>(&SPC700::regY,&SPC700::ReadImm); }
  void CMP_Y_dp()      { return CMP<3>(&SPC700::regY,&SPC700::memDP); }
  void CMP_Y_abs()     { return CMP<4>(&SPC700::regY,&SPC700::memAbs); }

  template<uint8_t cycles,class OP1,class OP2>
  void AND(OP1 op1, OP2 op2) {
    uint8_t& m = (this->*op1)();
    uint8_t n =  (this->*op2)();
    uint16_t result = m & n;
    UpdateFlags<FlagN|FlagZ>(result,m,n);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void AND_A_imm()     { return AND<2>(&SPC700::regA,&SPC700::ReadImm); }
  void AND_A_Xind()    { return AND<3>(&SPC700::regA,&SPC700::memXIndirect); }
  void AND_A_dp()      { return AND<3>(&SPC700::regA,&SPC700::memDP); }
  void AND_A_dpX()     { return AND<4>(&SPC700::regA,&SPC700::memDPX); }
  void AND_A_abs()     { return AND<4>(&SPC700::regA,&SPC700::memAbs); }
  void AND_A_absX()    { return AND<5>(&SPC700::regA,&SPC700::memAbsX); }
  void AND_A_absY()    { return AND<5>(&SPC700::regA,&SPC700::memAbsY); }
  void AND_A_dpXind()  { return AND<6>(&SPC700::regA,&SPC700::memDPXIndirect); }
  void AND_A_dpYind()  { return AND<6>(&SPC700::regA,&SPC700::memDPYYndirect); }
  void AND_Xind_Yind() { return AND<5>(&SPC700::memXIndirect,&SPC700::memYIndirect); }
  void AND_dp_dp()     { return AND<6>(&SPC700::memDP,&SPC700::memDP); }
  void AND_dp_imm()    { return AND<5>(&SPC700::memDP,&SPC700::ReadImm); }

  template<uint8_t cycles,class OP1,class OP2>
  void OR(OP1 op1, OP2 op2) {
    uint8_t& m = (this->*op1)();
    uint8_t n =  (this->*op2)();
    uint16_t result = m | n;
    UpdateFlags<FlagN|FlagZ>(result,m,n);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void OR_A_imm()     { return OR<2>(&SPC700::regA,&SPC700::ReadImm); }
  void OR_A_Xind()    { return OR<3>(&SPC700::regA,&SPC700::memXIndirect); }
  void OR_A_dp()      { return OR<3>(&SPC700::regA,&SPC700::memDP); }
  void OR_A_dpX()     { return OR<4>(&SPC700::regA,&SPC700::memDPX); }
  void OR_A_abs()     { return OR<4>(&SPC700::regA,&SPC700::memAbs); }
  void OR_A_absX()    { return OR<5>(&SPC700::regA,&SPC700::memAbsX); }
  void OR_A_absY()    { return OR<5>(&SPC700::regA,&SPC700::memAbsY); }
  void OR_A_dpXind()  { return OR<6>(&SPC700::regA,&SPC700::memDPXIndirect); }
  void OR_A_dpYind()  { return OR<6>(&SPC700::regA,&SPC700::memDPYYndirect); }
  void OR_Xind_Yind() { return OR<5>(&SPC700::memXIndirect,&SPC700::memYIndirect); }
  void OR_dp_dp()     { return OR<6>(&SPC700::memDP,&SPC700::memDP); }
  void OR_dp_imm()    { return OR<5>(&SPC700::memDP,&SPC700::ReadImm); }

  template<uint8_t cycles,class OP1,class OP2>
  void EOR(OP1 op1, OP2 op2) {
    uint8_t& m = (this->*op1)();
    uint8_t n =  (this->*op2)();
    uint16_t result = m ^ n;
    UpdateFlags<FlagN|FlagZ>(result,m,n);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void EOR_A_imm()     { return EOR<2>(&SPC700::regA,&SPC700::ReadImm); }
  void EOR_A_Xind()    { return EOR<3>(&SPC700::regA,&SPC700::memXIndirect); }
  void EOR_A_dp()      { return EOR<3>(&SPC700::regA,&SPC700::memDP); }
  void EOR_A_dpX()     { return EOR<4>(&SPC700::regA,&SPC700::memDPX); }
  void EOR_A_abs()     { return EOR<4>(&SPC700::regA,&SPC700::memAbs); }
  void EOR_A_absX()    { return EOR<5>(&SPC700::regA,&SPC700::memAbsX); }
  void EOR_A_absY()    { return EOR<5>(&SPC700::regA,&SPC700::memAbsY); }
  void EOR_A_dpXind()  { return EOR<6>(&SPC700::regA,&SPC700::memDPXIndirect); }
  void EOR_A_dpYind()  { return EOR<6>(&SPC700::regA,&SPC700::memDPYYndirect); }
  void EOR_Xind_Yind() { return EOR<5>(&SPC700::memXIndirect,&SPC700::memYIndirect); }
  void EOR_dp_dp()     { return EOR<6>(&SPC700::memDP,&SPC700::memDP); }
  void EOR_dp_imm()    { return EOR<5>(&SPC700::memDP,&SPC700::ReadImm); }

  template<uint8_t cycles,class OP1>
  void INC(OP1 op1) {
    uint8_t& m = (this->*op1)();
    uint16_t result = ++m;
    UpdateFlags<FlagN|FlagZ>(result,m,0);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void INC_A()     { return INC<2>(&SPC700::regA); }
  void INC_dp()    { return INC<4>(&SPC700::memDP); }
  void INC_dpX()   { return INC<5>(&SPC700::memDPX); }
  void INC_abs()   { return INC<5>(&SPC700::memAbs); }
  void INC_X()     { return INC<2>(&SPC700::regX); }
  void INC_Y()     { return INC<2>(&SPC700::regY); }

  template<uint8_t cycles,class OP1>
  void DEC(OP1 op1) {
    uint8_t& m = (this->*op1)();
    uint16_t result = --m;
    UpdateFlags<FlagN|FlagZ>(result,m,0);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void DEC_A()     { return DEC<2>(&SPC700::regA); }
  void DEC_dp()    { return DEC<4>(&SPC700::memDP); }
  void DEC_dpX()   { return DEC<5>(&SPC700::memDPX); }
  void DEC_abs()   { return DEC<5>(&SPC700::memAbs); }
  void DEC_X()     { return DEC<2>(&SPC700::regX); }
  void DEC_Y()     { return DEC<2>(&SPC700::regY); }

  template<uint8_t cycles,class OP1>
  void ASL(OP1 op1) {
    uint8_t& m = (this->*op1)();
    reg.PSW.C = m & 0x80;
    uint16_t result = m << 1;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m,0);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void ASL_A()     { return ASL<2>(&SPC700::regA); }
  void ASL_dp()    { return ASL<4>(&SPC700::memDP); }
  void ASL_dpX()   { return ASL<5>(&SPC700::memDPX); }
  void ASL_abs()   { return ASL<5>(&SPC700::memAbs); }

  template<uint8_t cycles,class OP1>
  void LSR(OP1 op1) {
    uint8_t& m = (this->*op1)();
    reg.PSW.C = m & 1;
    uint16_t result = m >> 1;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m,0);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void LSR_A()     { return LSR<2>(&SPC700::regA); }
  void LSR_dp()    { return LSR<4>(&SPC700::memDP); }
  void LSR_dpX()   { return LSR<5>(&SPC700::memDPX); }
  void LSR_abs()   { return LSR<5>(&SPC700::memAbs); }

  template<uint8_t cycles,class OP1>
  void ROL(OP1 op1) {
    uint8_t& m = (this->*op1)();
    uint8_t oldC = reg.PSW.C;
    reg.PSW.C = m & 0x80;
    uint16_t result = (m << 1) | oldC;
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m,0);
    m = result  & 0xFF;
    cycles_ += cycles;
  }

  void ROL_A()     { return ROL<2>(&SPC700::regA); }
  void ROL_dp()    { return ROL<4>(&SPC700::memDP); }
  void ROL_dpX()   { return ROL<5>(&SPC700::memDPX); }
  void ROL_abs()   { return ROL<5>(&SPC700::memAbs); }

  template<uint8_t cycles,class OP1>
  void ROR(OP1 op1) {
    uint8_t& m = (this->*op1)();
    uint8_t oldC = reg.PSW.C;
    reg.PSW.C = m & 0x1;
    uint16_t result = (m >> 1) | (oldC<<7);
    UpdateFlags<FlagN|FlagZ|FlagC>(result,m,0);
    m = result & 0xFF;
    cycles_ += cycles;
  }

  void ROR_A()     { return ROR<2>(&SPC700::regA); }
  void ROR_dp()    { return ROR<4>(&SPC700::memDP); }
  void ROR_dpX()   { return ROR<5>(&SPC700::memDPX); }
  void ROR_abs()   { return ROR<5>(&SPC700::memAbs); }

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


  template<uint16_t addr>
  void TCALL() { reg.PC = addr; cycles_+=8; }




  void DI() { /* no interrupts anyway */ }

  void NOP() { }
  void SLEEP() { sleep_ = true; }
  void STOP() { stop_ = true; }
};

}
}
}
}