//===-- AMDGPUInstPrinter.h - AMDGPU MC Inst -> ASM interface ---*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
/// \file
//===----------------------------------------------------------------------===//

#ifndef AMDGPUINSTPRINTER_H
#define AMDGPUINSTPRINTER_H

#include "llvm/ADT/StringRef.h"
#include "llvm/MC/MCInstPrinter.h"
#include "llvm/Support/raw_ostream.h"

namespace llvm {

class AMDGPUInstPrinter : public MCInstPrinter {
public:
  AMDGPUInstPrinter(const MCAsmInfo &MAI, const MCInstrInfo &MII,
                     const MCRegisterInfo &MRI)
    : MCInstPrinter(MAI, MII, MRI) {}

  //Autogenerated by tblgen
  void printInstruction(const MCInst *MI, raw_ostream &O);
  static const char *getRegisterName(unsigned RegNo);

  virtual void printInst(const MCInst *MI, raw_ostream &O, StringRef Annot);

private:
  void printOperand(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printMemOperand(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printIfSet(const MCInst *MI, unsigned OpNo, raw_ostream &O, StringRef Asm);
  void printAbs(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printClamp(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printLiteral(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printLast(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printNeg(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printOMOD(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printRel(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printUpdateExecMask(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printUpdatePred(const MCInst *MI, unsigned OpNo, raw_ostream &O);
  void printWrite(const MCInst *MI, unsigned OpNo, raw_ostream &O);
};

} // End namespace llvm

#endif // AMDGPUINSTRPRINTER_H
