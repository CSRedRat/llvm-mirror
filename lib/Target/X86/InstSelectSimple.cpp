//===-- InstSelectSimple.cpp - A simple instruction selector for x86 ------===//
//
// This file defines a simple peephole instruction selector for the x86 target
//
//===----------------------------------------------------------------------===//

#include "X86.h"
#include "X86InstrInfo.h"
#include "X86InstrBuilder.h"
#include "llvm/Function.h"
#include "llvm/iTerminators.h"
#include "llvm/iOperators.h"
#include "llvm/iOther.h"
#include "llvm/iPHINode.h"
#include "llvm/iMemory.h"
#include "llvm/Type.h"
#include "llvm/DerivedTypes.h"
#include "llvm/Constants.h"
#include "llvm/Pass.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "llvm/CodeGen/SSARegMap.h"
#include "llvm/CodeGen/MachineFrameInfo.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/InstVisitor.h"
#include "llvm/Target/MRegisterInfo.h"
#include <map>

/// BMI - A special BuildMI variant that takes an iterator to insert the
/// instruction at as well as a basic block.  This is the version for when you
/// have a destination register in mind.
inline static MachineInstrBuilder BMI(MachineBasicBlock *MBB,
                                      MachineBasicBlock::iterator &I,
                                      MachineOpCode Opcode,
                                      unsigned NumOperands,
                                      unsigned DestReg) {
  assert(I >= MBB->begin() && I <= MBB->end() && "Bad iterator!");
  MachineInstr *MI = new MachineInstr(Opcode, NumOperands+1, true, true);
  I = MBB->insert(I, MI)+1;
  return MachineInstrBuilder(MI).addReg(DestReg, MOTy::Def);
}

/// BMI - A special BuildMI variant that takes an iterator to insert the
/// instruction at as well as a basic block.
inline static MachineInstrBuilder BMI(MachineBasicBlock *MBB,
                                      MachineBasicBlock::iterator &I,
                                      MachineOpCode Opcode,
                                      unsigned NumOperands) {
  assert(I >= MBB->begin() && I <= MBB->end() && "Bad iterator!");
  MachineInstr *MI = new MachineInstr(Opcode, NumOperands, true, true);
  I = MBB->insert(I, MI)+1;
  return MachineInstrBuilder(MI);
}


namespace {
  struct ISel : public FunctionPass, InstVisitor<ISel> {
    TargetMachine &TM;
    MachineFunction *F;                    // The function we are compiling into
    MachineBasicBlock *BB;                 // The current MBB we are compiling

    std::map<Value*, unsigned> RegMap;  // Mapping between Val's and SSA Regs

    // MBBMap - Mapping between LLVM BB -> Machine BB
    std::map<const BasicBlock*, MachineBasicBlock*> MBBMap;

    ISel(TargetMachine &tm) : TM(tm), F(0), BB(0) {}

    /// runOnFunction - Top level implementation of instruction selection for
    /// the entire function.
    ///
    bool runOnFunction(Function &Fn) {
      F = &MachineFunction::construct(&Fn, TM);

      // Create all of the machine basic blocks for the function...
      for (Function::iterator I = Fn.begin(), E = Fn.end(); I != E; ++I)
        F->getBasicBlockList().push_back(MBBMap[I] = new MachineBasicBlock(I));

      BB = &F->front();
      LoadArgumentsToVirtualRegs(Fn);

      // Instruction select everything except PHI nodes
      visit(Fn);

      // Select the PHI nodes
      SelectPHINodes();

      RegMap.clear();
      MBBMap.clear();
      F = 0;
      return false;  // We never modify the LLVM itself.
    }

    virtual const char *getPassName() const {
      return "X86 Simple Instruction Selection";
    }

    /// visitBasicBlock - This method is called when we are visiting a new basic
    /// block.  This simply creates a new MachineBasicBlock to emit code into
    /// and adds it to the current MachineFunction.  Subsequent visit* for
    /// instructions will be invoked for all instructions in the basic block.
    ///
    void visitBasicBlock(BasicBlock &LLVM_BB) {
      BB = MBBMap[&LLVM_BB];
    }

    /// LoadArgumentsToVirtualRegs - Load all of the arguments to this function
    /// from the stack into virtual registers.
    ///
    void LoadArgumentsToVirtualRegs(Function &F);

    /// SelectPHINodes - Insert machine code to generate phis.  This is tricky
    /// because we have to generate our sources into the source basic blocks,
    /// not the current one.
    ///
    void SelectPHINodes();

    // Visitation methods for various instructions.  These methods simply emit
    // fixed X86 code for each instruction.
    //

    // Control flow operators
    void visitReturnInst(ReturnInst &RI);
    void visitBranchInst(BranchInst &BI);

    struct ValueRecord {
      unsigned Reg;
      const Type *Ty;
      ValueRecord(unsigned R, const Type *T) : Reg(R), Ty(T) {}
    };
    void doCall(const ValueRecord &Ret, MachineInstr *CallMI,
		const std::vector<ValueRecord> &Args);
    void visitCallInst(CallInst &I);

    // Arithmetic operators
    void visitSimpleBinary(BinaryOperator &B, unsigned OpcodeClass);
    void visitAdd(BinaryOperator &B) { visitSimpleBinary(B, 0); }
    void visitSub(BinaryOperator &B) { visitSimpleBinary(B, 1); }
    void doMultiply(MachineBasicBlock *MBB, MachineBasicBlock::iterator &MBBI,
                    unsigned DestReg, const Type *DestTy,
		    unsigned Op0Reg, unsigned Op1Reg);
    void visitMul(BinaryOperator &B);

    void visitDiv(BinaryOperator &B) { visitDivRem(B); }
    void visitRem(BinaryOperator &B) { visitDivRem(B); }
    void visitDivRem(BinaryOperator &B);

    // Bitwise operators
    void visitAnd(BinaryOperator &B) { visitSimpleBinary(B, 2); }
    void visitOr (BinaryOperator &B) { visitSimpleBinary(B, 3); }
    void visitXor(BinaryOperator &B) { visitSimpleBinary(B, 4); }

    // Comparison operators...
    void visitSetCondInst(SetCondInst &I);
    bool EmitComparisonGetSignedness(unsigned OpNum, Value *Op0, Value *Op1);

    // Memory Instructions
    MachineInstr *doFPLoad(MachineBasicBlock *MBB,
			   MachineBasicBlock::iterator &MBBI,
			   const Type *Ty, unsigned DestReg);
    void visitLoadInst(LoadInst &I);
    void doFPStore(const Type *Ty, unsigned DestAddrReg, unsigned SrcReg);
    void visitStoreInst(StoreInst &I);
    void visitGetElementPtrInst(GetElementPtrInst &I);
    void visitAllocaInst(AllocaInst &I);
    void visitMallocInst(MallocInst &I);
    void visitFreeInst(FreeInst &I);
    
    // Other operators
    void visitShiftInst(ShiftInst &I);
    void visitPHINode(PHINode &I) {}      // PHI nodes handled by second pass
    void visitCastInst(CastInst &I);

    void visitInstruction(Instruction &I) {
      std::cerr << "Cannot instruction select: " << I;
      abort();
    }

    /// promote32 - Make a value 32-bits wide, and put it somewhere.
    ///
    void promote32(unsigned targetReg, const ValueRecord &VR);

    /// EmitByteSwap - Byteswap SrcReg into DestReg.
    ///
    void EmitByteSwap(unsigned DestReg, unsigned SrcReg, unsigned Class);
    
    /// emitGEPOperation - Common code shared between visitGetElementPtrInst and
    /// constant expression GEP support.
    ///
    void emitGEPOperation(MachineBasicBlock *BB, MachineBasicBlock::iterator&IP,
                          Value *Src, User::op_iterator IdxBegin,
                          User::op_iterator IdxEnd, unsigned TargetReg);

    /// emitCastOperation - Common code shared between visitCastInst and
    /// constant expression cast support.
    void emitCastOperation(MachineBasicBlock *BB,MachineBasicBlock::iterator&IP,
                           Value *Src, const Type *DestTy, unsigned TargetReg);

    /// copyConstantToRegister - Output the instructions required to put the
    /// specified constant into the specified register.
    ///
    void copyConstantToRegister(MachineBasicBlock *MBB,
                                MachineBasicBlock::iterator &MBBI,
                                Constant *C, unsigned Reg);

    /// makeAnotherReg - This method returns the next register number we haven't
    /// yet used.
    ///
    /// Long values are handled somewhat specially.  They are always allocated
    /// as pairs of 32 bit integer values.  The register number returned is the
    /// lower 32 bits of the long value, and the regNum+1 is the upper 32 bits
    /// of the long value.
    ///
    unsigned makeAnotherReg(const Type *Ty) {
      if (Ty == Type::LongTy || Ty == Type::ULongTy) {
	const TargetRegisterClass *RC =
	  TM.getRegisterInfo()->getRegClassForType(Type::IntTy);
	// Create the lower part
	F->getSSARegMap()->createVirtualRegister(RC);
	// Create the upper part.
	return F->getSSARegMap()->createVirtualRegister(RC)-1;
      }

      // Add the mapping of regnumber => reg class to MachineFunction
      const TargetRegisterClass *RC =
	TM.getRegisterInfo()->getRegClassForType(Ty);
      return F->getSSARegMap()->createVirtualRegister(RC);
    }

    /// getReg - This method turns an LLVM value into a register number.  This
    /// is guaranteed to produce the same register number for a particular value
    /// every time it is queried.
    ///
    unsigned getReg(Value &V) { return getReg(&V); }  // Allow references
    unsigned getReg(Value *V) {
      // Just append to the end of the current bb.
      MachineBasicBlock::iterator It = BB->end();
      return getReg(V, BB, It);
    }
    unsigned getReg(Value *V, MachineBasicBlock *MBB,
                    MachineBasicBlock::iterator &IPt) {
      unsigned &Reg = RegMap[V];
      if (Reg == 0) {
        Reg = makeAnotherReg(V->getType());
        RegMap[V] = Reg;
      }

      // If this operand is a constant, emit the code to copy the constant into
      // the register here...
      //
      if (Constant *C = dyn_cast<Constant>(V)) {
        copyConstantToRegister(MBB, IPt, C, Reg);
        RegMap.erase(V);  // Assign a new name to this constant if ref'd again
      } else if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
        // Move the address of the global into the register
        BMI(MBB, IPt, X86::MOVir32, 1, Reg).addGlobalAddress(GV);
        RegMap.erase(V);  // Assign a new name to this address if ref'd again
      }

      return Reg;
    }
  };
}

/// TypeClass - Used by the X86 backend to group LLVM types by their basic X86
/// Representation.
///
enum TypeClass {
  cByte, cShort, cInt, cFP, cLong
};

/// getClass - Turn a primitive type into a "class" number which is based on the
/// size of the type, and whether or not it is floating point.
///
static inline TypeClass getClass(const Type *Ty) {
  switch (Ty->getPrimitiveID()) {
  case Type::SByteTyID:
  case Type::UByteTyID:   return cByte;      // Byte operands are class #0
  case Type::ShortTyID:
  case Type::UShortTyID:  return cShort;     // Short operands are class #1
  case Type::IntTyID:
  case Type::UIntTyID:
  case Type::PointerTyID: return cInt;       // Int's and pointers are class #2

  case Type::FloatTyID:
  case Type::DoubleTyID:  return cFP;        // Floating Point is #3

  case Type::LongTyID:
  case Type::ULongTyID:   return cLong;      // Longs are class #4
  default:
    assert(0 && "Invalid type to getClass!");
    return cByte;  // not reached
  }
}

// getClassB - Just like getClass, but treat boolean values as bytes.
static inline TypeClass getClassB(const Type *Ty) {
  if (Ty == Type::BoolTy) return cByte;
  return getClass(Ty);
}


/// copyConstantToRegister - Output the instructions required to put the
/// specified constant into the specified register.
///
void ISel::copyConstantToRegister(MachineBasicBlock *MBB,
                                  MachineBasicBlock::iterator &IP,
                                  Constant *C, unsigned R) {
  if (ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
    if (CE->getOpcode() == Instruction::GetElementPtr) {
      emitGEPOperation(MBB, IP, CE->getOperand(0),
                       CE->op_begin()+1, CE->op_end(), R);
      return;
    } else if (CE->getOpcode() == Instruction::Cast) {
      emitCastOperation(MBB, IP, CE->getOperand(0), CE->getType(), R);
      return;
    }

    std::cerr << "Offending expr: " << C << "\n";
    assert(0 && "Constant expressions not yet handled!\n");
  }

  if (C->getType()->isIntegral()) {
    unsigned Class = getClassB(C->getType());

    if (Class == cLong) {
      // Copy the value into the register pair.
      uint64_t Val;
      if (C->getType()->isSigned())
	Val = cast<ConstantSInt>(C)->getValue();
      else
	Val = cast<ConstantUInt>(C)->getValue();

      BMI(MBB, IP, X86::MOVir32, 1, R).addZImm(Val & 0xFFFFFFFF);
      BMI(MBB, IP, X86::MOVir32, 1, R+1).addZImm(Val >> 32);
      return;
    }

    assert(Class <= cInt && "Type not handled yet!");

    static const unsigned IntegralOpcodeTab[] = {
      X86::MOVir8, X86::MOVir16, X86::MOVir32
    };

    if (C->getType() == Type::BoolTy) {
      BMI(MBB, IP, X86::MOVir8, 1, R).addZImm(C == ConstantBool::True);
    } else if (C->getType()->isSigned()) {
      ConstantSInt *CSI = cast<ConstantSInt>(C);
      BMI(MBB, IP, IntegralOpcodeTab[Class], 1, R).addZImm(CSI->getValue());
    } else {
      ConstantUInt *CUI = cast<ConstantUInt>(C);
      BMI(MBB, IP, IntegralOpcodeTab[Class], 1, R).addZImm(CUI->getValue());
    }
  } else if (ConstantFP *CFP = dyn_cast<ConstantFP>(C)) {
    double Value = CFP->getValue();
    if (Value == +0.0)
      BMI(MBB, IP, X86::FLD0, 0, R);
    else if (Value == +1.0)
      BMI(MBB, IP, X86::FLD1, 0, R);
    else {
      // Otherwise we need to spill the constant to memory...
      MachineConstantPool *CP = F->getConstantPool();
      unsigned CPI = CP->getConstantPoolIndex(CFP);
      addConstantPoolReference(doFPLoad(MBB, IP, CFP->getType(), R), CPI);
    }

  } else if (isa<ConstantPointerNull>(C)) {
    // Copy zero (null pointer) to the register.
    BMI(MBB, IP, X86::MOVir32, 1, R).addZImm(0);
  } else if (ConstantPointerRef *CPR = dyn_cast<ConstantPointerRef>(C)) {
    unsigned SrcReg = getReg(CPR->getValue(), MBB, IP);
    BMI(MBB, IP, X86::MOVrr32, 1, R).addReg(SrcReg);
  } else {
    std::cerr << "Offending constant: " << C << "\n";
    assert(0 && "Type not handled yet!");
  }
}

/// LoadArgumentsToVirtualRegs - Load all of the arguments to this function from
/// the stack into virtual registers.
///
void ISel::LoadArgumentsToVirtualRegs(Function &Fn) {
  // Emit instructions to load the arguments...  On entry to a function on the
  // X86, the stack frame looks like this:
  //
  // [ESP] -- return address
  // [ESP + 4] -- first argument (leftmost lexically)
  // [ESP + 8] -- second argument, if first argument is four bytes in size
  //    ... 
  //
  unsigned ArgOffset = 0;   // Frame mechanisms handle retaddr slot
  MachineFrameInfo *MFI = F->getFrameInfo();

  for (Function::aiterator I = Fn.abegin(), E = Fn.aend(); I != E; ++I) {
    unsigned Reg = getReg(*I);
    
    int FI;          // Frame object index
    switch (getClassB(I->getType())) {
    case cByte:
      FI = MFI->CreateFixedObject(1, ArgOffset);
      addFrameReference(BuildMI(BB, X86::MOVmr8, 4, Reg), FI);
      break;
    case cShort:
      FI = MFI->CreateFixedObject(2, ArgOffset);
      addFrameReference(BuildMI(BB, X86::MOVmr16, 4, Reg), FI);
      break;
    case cInt:
      FI = MFI->CreateFixedObject(4, ArgOffset);
      addFrameReference(BuildMI(BB, X86::MOVmr32, 4, Reg), FI);
      break;
    case cLong:
      FI = MFI->CreateFixedObject(8, ArgOffset);
      addFrameReference(BuildMI(BB, X86::MOVmr32, 4, Reg), FI);
      addFrameReference(BuildMI(BB, X86::MOVmr32, 4, Reg+1), FI, 4);
      ArgOffset += 4;   // longs require 4 additional bytes
      break;
    case cFP:
      unsigned Opcode;
      if (I->getType() == Type::FloatTy) {
	Opcode = X86::FLDr32;
	FI = MFI->CreateFixedObject(4, ArgOffset);
      } else {
	Opcode = X86::FLDr64;
	FI = MFI->CreateFixedObject(8, ArgOffset);
	ArgOffset += 4;   // doubles require 4 additional bytes
      }
      addFrameReference(BuildMI(BB, Opcode, 4, Reg), FI);
      break;
    default:
      assert(0 && "Unhandled argument type!");
    }
    ArgOffset += 4;  // Each argument takes at least 4 bytes on the stack...
  }
}


/// SelectPHINodes - Insert machine code to generate phis.  This is tricky
/// because we have to generate our sources into the source basic blocks, not
/// the current one.
///
void ISel::SelectPHINodes() {
  const TargetInstrInfo &TII = TM.getInstrInfo();
  const Function &LF = *F->getFunction();  // The LLVM function...
  for (Function::const_iterator I = LF.begin(), E = LF.end(); I != E; ++I) {
    const BasicBlock *BB = I;
    MachineBasicBlock *MBB = MBBMap[I];

    // Loop over all of the PHI nodes in the LLVM basic block...
    unsigned NumPHIs = 0;
    for (BasicBlock::const_iterator I = BB->begin();
         PHINode *PN = (PHINode*)dyn_cast<PHINode>(I); ++I) {

      // Create a new machine instr PHI node, and insert it.
      unsigned PHIReg = getReg(*PN);
      MachineInstr *PhiMI = BuildMI(X86::PHI, PN->getNumOperands(), PHIReg);
      MBB->insert(MBB->begin()+NumPHIs++, PhiMI);

      MachineInstr *LongPhiMI = 0;
      if (PN->getType() == Type::LongTy || PN->getType() == Type::ULongTy) {
	LongPhiMI = BuildMI(X86::PHI, PN->getNumOperands(), PHIReg+1);
	MBB->insert(MBB->begin()+NumPHIs++, LongPhiMI);
      }

      for (unsigned i = 0, e = PN->getNumIncomingValues(); i != e; ++i) {
        MachineBasicBlock *PredMBB = MBBMap[PN->getIncomingBlock(i)];

        // Get the incoming value into a virtual register.  If it is not already
        // available in a virtual register, insert the computation code into
        // PredMBB
        //
	MachineBasicBlock::iterator PI = PredMBB->end();
	while (PI != PredMBB->begin() &&
	       TII.isTerminatorInstr((*(PI-1))->getOpcode()))
	  --PI;
	unsigned ValReg = getReg(PN->getIncomingValue(i), PredMBB, PI);
	PhiMI->addRegOperand(ValReg);
        PhiMI->addMachineBasicBlockOperand(PredMBB);
	if (LongPhiMI) {
	  LongPhiMI->addRegOperand(ValReg+1);
	  LongPhiMI->addMachineBasicBlockOperand(PredMBB);
	}
      }
    }
  }
}

// canFoldSetCCIntoBranch - Return the setcc instruction if we can fold it into
// the conditional branch instruction which is the only user of the cc
// instruction.  This is the case if the conditional branch is the only user of
// the setcc, and if the setcc is in the same basic block as the conditional
// branch.  We also don't handle long arguments below, so we reject them here as
// well.
//
static SetCondInst *canFoldSetCCIntoBranch(Value *V) {
  if (SetCondInst *SCI = dyn_cast<SetCondInst>(V))
    if (SCI->use_size() == 1 && isa<BranchInst>(SCI->use_back()) &&
        SCI->getParent() == cast<BranchInst>(SCI->use_back())->getParent()) {
      const Type *Ty = SCI->getOperand(0)->getType();
      if (Ty != Type::LongTy && Ty != Type::ULongTy)
        return SCI;
    }
  return 0;
}

// Return a fixed numbering for setcc instructions which does not depend on the
// order of the opcodes.
//
static unsigned getSetCCNumber(unsigned Opcode) {
  switch(Opcode) {
  default: assert(0 && "Unknown setcc instruction!");
  case Instruction::SetEQ: return 0;
  case Instruction::SetNE: return 1;
  case Instruction::SetLT: return 2;
  case Instruction::SetGE: return 3;
  case Instruction::SetGT: return 4;
  case Instruction::SetLE: return 5;
  }
}

// LLVM  -> X86 signed  X86 unsigned
// -----    ----------  ------------
// seteq -> sete        sete
// setne -> setne       setne
// setlt -> setl        setb
// setge -> setge       setae
// setgt -> setg        seta
// setle -> setle       setbe
static const unsigned SetCCOpcodeTab[2][6] = {
  {X86::SETEr, X86::SETNEr, X86::SETBr, X86::SETAEr, X86::SETAr, X86::SETBEr},
  {X86::SETEr, X86::SETNEr, X86::SETLr, X86::SETGEr, X86::SETGr, X86::SETLEr},
};

bool ISel::EmitComparisonGetSignedness(unsigned OpNum, Value *Op0, Value *Op1) {

  // The arguments are already supposed to be of the same type.
  const Type *CompTy = Op0->getType();
  bool isSigned = CompTy->isSigned();
  unsigned reg1 = getReg(Op0);
  unsigned reg2 = getReg(Op1);

  unsigned Class = getClassB(CompTy);
  switch (Class) {
  default: assert(0 && "Unknown type class!");
    // Emit: cmp <var1>, <var2> (do the comparison).  We can
    // compare 8-bit with 8-bit, 16-bit with 16-bit, 32-bit with
    // 32-bit.
  case cByte:
    BuildMI(BB, X86::CMPrr8, 2).addReg(reg1).addReg(reg2);
    break;
  case cShort:
    BuildMI(BB, X86::CMPrr16, 2).addReg(reg1).addReg(reg2);
    break;
  case cInt:
    BuildMI(BB, X86::CMPrr32, 2).addReg(reg1).addReg(reg2);
    break;
  case cFP:
    BuildMI(BB, X86::FpUCOM, 2).addReg(reg1).addReg(reg2);
    BuildMI(BB, X86::FNSTSWr8, 0);
    BuildMI(BB, X86::SAHF, 1);
    isSigned = false;   // Compare with unsigned operators
    break;

  case cLong:
    if (OpNum < 2) {    // seteq, setne
      unsigned LoTmp = makeAnotherReg(Type::IntTy);
      unsigned HiTmp = makeAnotherReg(Type::IntTy);
      unsigned FinalTmp = makeAnotherReg(Type::IntTy);
      BuildMI(BB, X86::XORrr32, 2, LoTmp).addReg(reg1).addReg(reg2);
      BuildMI(BB, X86::XORrr32, 2, HiTmp).addReg(reg1+1).addReg(reg2+1);
      BuildMI(BB, X86::ORrr32,  2, FinalTmp).addReg(LoTmp).addReg(HiTmp);
      break;  // Allow the sete or setne to be generated from flags set by OR
    } else {
      // Emit a sequence of code which compares the high and low parts once
      // each, then uses a conditional move to handle the overflow case.  For
      // example, a setlt for long would generate code like this:
      //
      // AL = lo(op1) < lo(op2)   // Signedness depends on operands
      // BL = hi(op1) < hi(op2)   // Always unsigned comparison
      // dest = hi(op1) == hi(op2) ? AL : BL;
      //

      // FIXME: This would be much better if we had hierarchical register
      // classes!  Until then, hardcode registers so that we can deal with their
      // aliases (because we don't have conditional byte moves).
      //
      BuildMI(BB, X86::CMPrr32, 2).addReg(reg1).addReg(reg2);
      BuildMI(BB, SetCCOpcodeTab[0][OpNum], 0, X86::AL);
      BuildMI(BB, X86::CMPrr32, 2).addReg(reg1+1).addReg(reg2+1);
      BuildMI(BB, SetCCOpcodeTab[isSigned][OpNum], 0, X86::BL);
      BuildMI(BB, X86::CMOVErr16, 2, X86::BX).addReg(X86::BX).addReg(X86::AX);
      // NOTE: visitSetCondInst knows that the value is dumped into the BL
      // register at this point for long values...
      return isSigned;
    }
  }
  return isSigned;
}


/// SetCC instructions - Here we just emit boilerplate code to set a byte-sized
/// register, then move it to wherever the result should be. 
///
void ISel::visitSetCondInst(SetCondInst &I) {
  if (canFoldSetCCIntoBranch(&I)) return;  // Fold this into a branch...

  unsigned OpNum = getSetCCNumber(I.getOpcode());
  unsigned DestReg = getReg(I);
  bool isSigned = EmitComparisonGetSignedness(OpNum, I.getOperand(0),
                                              I.getOperand(1));

  if (getClassB(I.getOperand(0)->getType()) != cLong || OpNum < 2) {
    // Handle normal comparisons with a setcc instruction...
    BuildMI(BB, SetCCOpcodeTab[isSigned][OpNum], 0, DestReg);
  } else {
    // Handle long comparisons by copying the value which is already in BL into
    // the register we want...
    BuildMI(BB, X86::MOVrr8, 1, DestReg).addReg(X86::BL);
  }
}

/// promote32 - Emit instructions to turn a narrow operand into a 32-bit-wide
/// operand, in the specified target register.
void ISel::promote32(unsigned targetReg, const ValueRecord &VR) {
  bool isUnsigned = VR.Ty->isUnsigned();
  switch (getClassB(VR.Ty)) {
  case cByte:
    // Extend value into target register (8->32)
    if (isUnsigned)
      BuildMI(BB, X86::MOVZXr32r8, 1, targetReg).addReg(VR.Reg);
    else
      BuildMI(BB, X86::MOVSXr32r8, 1, targetReg).addReg(VR.Reg);
    break;
  case cShort:
    // Extend value into target register (16->32)
    if (isUnsigned)
      BuildMI(BB, X86::MOVZXr32r16, 1, targetReg).addReg(VR.Reg);
    else
      BuildMI(BB, X86::MOVSXr32r16, 1, targetReg).addReg(VR.Reg);
    break;
  case cInt:
    // Move value into target register (32->32)
    BuildMI(BB, X86::MOVrr32, 1, targetReg).addReg(VR.Reg);
    break;
  default:
    assert(0 && "Unpromotable operand class in promote32");
  }
}

/// 'ret' instruction - Here we are interested in meeting the x86 ABI.  As such,
/// we have the following possibilities:
///
///   ret void: No return value, simply emit a 'ret' instruction
///   ret sbyte, ubyte : Extend value into EAX and return
///   ret short, ushort: Extend value into EAX and return
///   ret int, uint    : Move value into EAX and return
///   ret pointer      : Move value into EAX and return
///   ret long, ulong  : Move value into EAX/EDX and return
///   ret float/double : Top of FP stack
///
void ISel::visitReturnInst(ReturnInst &I) {
  if (I.getNumOperands() == 0) {
    BuildMI(BB, X86::RET, 0); // Just emit a 'ret' instruction
    return;
  }

  Value *RetVal = I.getOperand(0);
  unsigned RetReg = getReg(RetVal);
  switch (getClassB(RetVal->getType())) {
  case cByte:   // integral return values: extend or move into EAX and return
  case cShort:
  case cInt:
    promote32(X86::EAX, ValueRecord(RetReg, RetVal->getType()));
    break;
  case cFP:                   // Floats & Doubles: Return in ST(0)
    BuildMI(BB, X86::FpSETRESULT, 1).addReg(RetReg);
    break;
  case cLong:
    BuildMI(BB, X86::MOVrr32, 1, X86::EAX).addReg(RetReg);
    BuildMI(BB, X86::MOVrr32, 1, X86::EDX).addReg(RetReg+1);
    break;
  default:
    visitInstruction(I);
  }
  // Emit a 'ret' instruction
  BuildMI(BB, X86::RET, 0);
}

// getBlockAfter - Return the basic block which occurs lexically after the
// specified one.
static inline BasicBlock *getBlockAfter(BasicBlock *BB) {
  Function::iterator I = BB; ++I;  // Get iterator to next block
  return I != BB->getParent()->end() ? &*I : 0;
}

/// visitBranchInst - Handle conditional and unconditional branches here.  Note
/// that since code layout is frozen at this point, that if we are trying to
/// jump to a block that is the immediate successor of the current block, we can
/// just make a fall-through (but we don't currently).
///
void ISel::visitBranchInst(BranchInst &BI) {
  BasicBlock *NextBB = getBlockAfter(BI.getParent());  // BB after current one

  if (!BI.isConditional()) {  // Unconditional branch?
    if (BI.getSuccessor(0) != NextBB)
      BuildMI(BB, X86::JMP, 1).addPCDisp(BI.getSuccessor(0));
    return;
  }

  // See if we can fold the setcc into the branch itself...
  SetCondInst *SCI = canFoldSetCCIntoBranch(BI.getCondition());
  if (SCI == 0) {
    // Nope, cannot fold setcc into this branch.  Emit a branch on a condition
    // computed some other way...
    unsigned condReg = getReg(BI.getCondition());
    BuildMI(BB, X86::CMPri8, 2).addReg(condReg).addZImm(0);
    if (BI.getSuccessor(1) == NextBB) {
      if (BI.getSuccessor(0) != NextBB)
        BuildMI(BB, X86::JNE, 1).addPCDisp(BI.getSuccessor(0));
    } else {
      BuildMI(BB, X86::JE, 1).addPCDisp(BI.getSuccessor(1));
      
      if (BI.getSuccessor(0) != NextBB)
        BuildMI(BB, X86::JMP, 1).addPCDisp(BI.getSuccessor(0));
    }
    return;
  }

  unsigned OpNum = getSetCCNumber(SCI->getOpcode());
  bool isSigned = EmitComparisonGetSignedness(OpNum, SCI->getOperand(0),
                                              SCI->getOperand(1));
  
  // LLVM  -> X86 signed  X86 unsigned
  // -----    ----------  ------------
  // seteq -> je          je
  // setne -> jne         jne
  // setlt -> jl          jb
  // setge -> jge         jae
  // setgt -> jg          ja
  // setle -> jle         jbe
  static const unsigned OpcodeTab[2][6] = {
    { X86::JE, X86::JNE, X86::JB, X86::JAE, X86::JA, X86::JBE },
    { X86::JE, X86::JNE, X86::JL, X86::JGE, X86::JG, X86::JLE },
  };
  
  if (BI.getSuccessor(0) != NextBB) {
    BuildMI(BB, OpcodeTab[isSigned][OpNum], 1).addPCDisp(BI.getSuccessor(0));
    if (BI.getSuccessor(1) != NextBB)
      BuildMI(BB, X86::JMP, 1).addPCDisp(BI.getSuccessor(1));
  } else {
    // Change to the inverse condition...
    if (BI.getSuccessor(1) != NextBB) {
      OpNum ^= 1;
      BuildMI(BB, OpcodeTab[isSigned][OpNum], 1).addPCDisp(BI.getSuccessor(1));
    }
  }
}


/// doCall - This emits an abstract call instruction, setting up the arguments
/// and the return value as appropriate.  For the actual function call itself,
/// it inserts the specified CallMI instruction into the stream.
///
void ISel::doCall(const ValueRecord &Ret, MachineInstr *CallMI,
		  const std::vector<ValueRecord> &Args) {

  // Count how many bytes are to be pushed on the stack...
  unsigned NumBytes = 0;

  if (!Args.empty()) {
    for (unsigned i = 0, e = Args.size(); i != e; ++i)
      switch (getClassB(Args[i].Ty)) {
      case cByte: case cShort: case cInt:
	NumBytes += 4; break;
      case cLong:
	NumBytes += 8; break;
      case cFP:
	NumBytes += Args[i].Ty == Type::FloatTy ? 4 : 8;
	break;
      default: assert(0 && "Unknown class!");
      }

    // Adjust the stack pointer for the new arguments...
    BuildMI(BB, X86::ADJCALLSTACKDOWN, 1).addZImm(NumBytes);

    // Arguments go on the stack in reverse order, as specified by the ABI.
    unsigned ArgOffset = 0;
    for (unsigned i = 0, e = Args.size(); i != e; ++i) {
      unsigned ArgReg = Args[i].Reg;
      switch (getClassB(Args[i].Ty)) {
      case cByte:
      case cShort: {
	// Promote arg to 32 bits wide into a temporary register...
	unsigned R = makeAnotherReg(Type::UIntTy);
	promote32(R, Args[i]);
	addRegOffset(BuildMI(BB, X86::MOVrm32, 5),
		     X86::ESP, ArgOffset).addReg(R);
	break;
      }
      case cInt:
	addRegOffset(BuildMI(BB, X86::MOVrm32, 5),
		     X86::ESP, ArgOffset).addReg(ArgReg);
	break;
      case cLong:
	addRegOffset(BuildMI(BB, X86::MOVrm32, 5),
		     X86::ESP, ArgOffset).addReg(ArgReg);
	addRegOffset(BuildMI(BB, X86::MOVrm32, 5),
		     X86::ESP, ArgOffset+4).addReg(ArgReg+1);
	ArgOffset += 4;        // 8 byte entry, not 4.
	break;
	
      case cFP:
	if (Args[i].Ty == Type::FloatTy) {
	  addRegOffset(BuildMI(BB, X86::FSTr32, 5),
		       X86::ESP, ArgOffset).addReg(ArgReg);
	} else {
	  assert(Args[i].Ty == Type::DoubleTy && "Unknown FP type!");
	  addRegOffset(BuildMI(BB, X86::FSTr64, 5),
		       X86::ESP, ArgOffset).addReg(ArgReg);
	  ArgOffset += 4;       // 8 byte entry, not 4.
	}
	break;

      default: assert(0 && "Unknown class!");
      }
      ArgOffset += 4;
    }
  } else {
    BuildMI(BB, X86::ADJCALLSTACKDOWN, 1).addZImm(0);
  }

  BB->push_back(CallMI);

  BuildMI(BB, X86::ADJCALLSTACKUP, 1).addZImm(NumBytes);

  // If there is a return value, scavenge the result from the location the call
  // leaves it in...
  //
  if (Ret.Ty != Type::VoidTy) {
    unsigned DestClass = getClassB(Ret.Ty);
    switch (DestClass) {
    case cByte:
    case cShort:
    case cInt: {
      // Integral results are in %eax, or the appropriate portion
      // thereof.
      static const unsigned regRegMove[] = {
	X86::MOVrr8, X86::MOVrr16, X86::MOVrr32
      };
      static const unsigned AReg[] = { X86::AL, X86::AX, X86::EAX };
      BuildMI(BB, regRegMove[DestClass], 1, Ret.Reg).addReg(AReg[DestClass]);
      break;
    }
    case cFP:     // Floating-point return values live in %ST(0)
      BuildMI(BB, X86::FpGETRESULT, 1, Ret.Reg);
      break;
    case cLong:   // Long values are left in EDX:EAX
      BuildMI(BB, X86::MOVrr32, 1, Ret.Reg).addReg(X86::EAX);
      BuildMI(BB, X86::MOVrr32, 1, Ret.Reg+1).addReg(X86::EDX);
      break;
    default: assert(0 && "Unknown class!");
    }
  }
}


/// visitCallInst - Push args on stack and do a procedure call instruction.
void ISel::visitCallInst(CallInst &CI) {
  MachineInstr *TheCall;
  if (Function *F = CI.getCalledFunction()) {
    // Emit a CALL instruction with PC-relative displacement.
    TheCall = BuildMI(X86::CALLpcrel32, 1).addGlobalAddress(F, true);
  } else {  // Emit an indirect call...
    unsigned Reg = getReg(CI.getCalledValue());
    TheCall = BuildMI(X86::CALLr32, 1).addReg(Reg);
  }

  std::vector<ValueRecord> Args;
  for (unsigned i = 1, e = CI.getNumOperands(); i != e; ++i)
    Args.push_back(ValueRecord(getReg(CI.getOperand(i)),
			       CI.getOperand(i)->getType()));

  unsigned DestReg = CI.getType() != Type::VoidTy ? getReg(CI) : 0;
  doCall(ValueRecord(DestReg, CI.getType()), TheCall, Args);
}	 


/// visitSimpleBinary - Implement simple binary operators for integral types...
/// OperatorClass is one of: 0 for Add, 1 for Sub, 2 for And, 3 for Or,
/// 4 for Xor.
///
void ISel::visitSimpleBinary(BinaryOperator &B, unsigned OperatorClass) {
  unsigned Class = getClassB(B.getType());

  static const unsigned OpcodeTab[][4] = {
    // Arithmetic operators
    { X86::ADDrr8, X86::ADDrr16, X86::ADDrr32, X86::FpADD },  // ADD
    { X86::SUBrr8, X86::SUBrr16, X86::SUBrr32, X86::FpSUB },  // SUB

    // Bitwise operators
    { X86::ANDrr8, X86::ANDrr16, X86::ANDrr32, 0 },  // AND
    { X86:: ORrr8, X86:: ORrr16, X86:: ORrr32, 0 },  // OR
    { X86::XORrr8, X86::XORrr16, X86::XORrr32, 0 },  // XOR
  };

  bool isLong = false;
  if (Class == cLong) {
    isLong = true;
    Class = cInt;          // Bottom 32 bits are handled just like ints
  }
  
  unsigned Opcode = OpcodeTab[OperatorClass][Class];
  assert(Opcode && "Floating point arguments to logical inst?");
  unsigned Op0r = getReg(B.getOperand(0));
  unsigned Op1r = getReg(B.getOperand(1));
  unsigned DestReg = getReg(B);
  BuildMI(BB, Opcode, 2, DestReg).addReg(Op0r).addReg(Op1r);

  if (isLong) {        // Handle the upper 32 bits of long values...
    static const unsigned TopTab[] = {
      X86::ADCrr32, X86::SBBrr32, X86::ANDrr32, X86::ORrr32, X86::XORrr32
    };
    BuildMI(BB, TopTab[OperatorClass], 2,
	    DestReg+1).addReg(Op0r+1).addReg(Op1r+1);
  }
}

/// doMultiply - Emit appropriate instructions to multiply together the
/// registers op0Reg and op1Reg, and put the result in DestReg.  The type of the
/// result should be given as DestTy.
///
/// FIXME: doMultiply should use one of the two address IMUL instructions!
///
void ISel::doMultiply(MachineBasicBlock *MBB, MachineBasicBlock::iterator &MBBI,
                      unsigned DestReg, const Type *DestTy,
                      unsigned op0Reg, unsigned op1Reg) {
  unsigned Class = getClass(DestTy);
  switch (Class) {
  case cFP:              // Floating point multiply
    BMI(BB, MBBI, X86::FpMUL, 2, DestReg).addReg(op0Reg).addReg(op1Reg);
    return;
  default:
  case cLong: assert(0 && "doMultiply cannot operate on LONG values!");
  case cByte:
  case cShort:
  case cInt:          // Small integerals, handled below...
    break;
  }
 
  static const unsigned Regs[]     ={ X86::AL    , X86::AX     , X86::EAX     };
  static const unsigned MulOpcode[]={ X86::MULr8 , X86::MULr16 , X86::MULr32  };
  static const unsigned MovOpcode[]={ X86::MOVrr8, X86::MOVrr16, X86::MOVrr32 };
  unsigned Reg     = Regs[Class];

  // Emit a MOV to put the first operand into the appropriately-sized
  // subreg of EAX.
  BMI(MBB, MBBI, MovOpcode[Class], 1, Reg).addReg(op0Reg);
  
  // Emit the appropriate multiply instruction.
  BMI(MBB, MBBI, MulOpcode[Class], 1).addReg(op1Reg);

  // Emit another MOV to put the result into the destination register.
  BMI(MBB, MBBI, MovOpcode[Class], 1, DestReg).addReg(Reg);
}

/// visitMul - Multiplies are not simple binary operators because they must deal
/// with the EAX register explicitly.
///
void ISel::visitMul(BinaryOperator &I) {
  unsigned Op0Reg  = getReg(I.getOperand(0));
  unsigned Op1Reg  = getReg(I.getOperand(1));
  unsigned DestReg = getReg(I);

  // Simple scalar multiply?
  if (I.getType() != Type::LongTy && I.getType() != Type::ULongTy) {
    MachineBasicBlock::iterator MBBI = BB->end();
    doMultiply(BB, MBBI, DestReg, I.getType(), Op0Reg, Op1Reg);
  } else {
    // Long value.  We have to do things the hard way...
    // Multiply the two low parts... capturing carry into EDX
    BuildMI(BB, X86::MOVrr32, 1, X86::EAX).addReg(Op0Reg);
    BuildMI(BB, X86::MULr32, 1).addReg(Op1Reg);  // AL*BL

    unsigned OverflowReg = makeAnotherReg(Type::UIntTy);
    BuildMI(BB, X86::MOVrr32, 1, DestReg).addReg(X86::EAX);     // AL*BL
    BuildMI(BB, X86::MOVrr32, 1, OverflowReg).addReg(X86::EDX); // AL*BL >> 32

    MachineBasicBlock::iterator MBBI = BB->end();
    unsigned AHBLReg = makeAnotherReg(Type::UIntTy);
    doMultiply(BB, MBBI, AHBLReg, Type::UIntTy, Op0Reg+1, Op1Reg); // AH*BL

    unsigned AHBLplusOverflowReg = makeAnotherReg(Type::UIntTy);
    BuildMI(BB, X86::ADDrr32, 2,                         // AH*BL+(AL*BL >> 32)
	    AHBLplusOverflowReg).addReg(AHBLReg).addReg(OverflowReg);
    
    MBBI = BB->end();
    unsigned ALBHReg = makeAnotherReg(Type::UIntTy);
    doMultiply(BB, MBBI, ALBHReg, Type::UIntTy, Op0Reg, Op1Reg+1); // AL*BH
    
    BuildMI(BB, X86::ADDrr32, 2,               // AL*BH + AH*BL + (AL*BL >> 32)
	    DestReg+1).addReg(AHBLplusOverflowReg).addReg(ALBHReg);
  }
}


/// visitDivRem - Handle division and remainder instructions... these
/// instruction both require the same instructions to be generated, they just
/// select the result from a different register.  Note that both of these
/// instructions work differently for signed and unsigned operands.
///
void ISel::visitDivRem(BinaryOperator &I) {
  unsigned Class     = getClass(I.getType());
  unsigned Op0Reg    = getReg(I.getOperand(0));
  unsigned Op1Reg    = getReg(I.getOperand(1));
  unsigned ResultReg = getReg(I);

  switch (Class) {
  case cFP:              // Floating point divide
    if (I.getOpcode() == Instruction::Div)
      BuildMI(BB, X86::FpDIV, 2, ResultReg).addReg(Op0Reg).addReg(Op1Reg);
    else {               // Floating point remainder...
      MachineInstr *TheCall =
	BuildMI(X86::CALLpcrel32, 1).addExternalSymbol("fmod", true);
      std::vector<ValueRecord> Args;
      Args.push_back(ValueRecord(Op0Reg, Type::DoubleTy));
      Args.push_back(ValueRecord(Op1Reg, Type::DoubleTy));
      doCall(ValueRecord(ResultReg, Type::DoubleTy), TheCall, Args);
    }
    return;
  case cLong: {
    static const char *FnName[] =
      { "__moddi3", "__divdi3", "__umoddi3", "__udivdi3" };

    unsigned NameIdx = I.getType()->isUnsigned()*2;
    NameIdx += I.getOpcode() == Instruction::Div;
    MachineInstr *TheCall =
      BuildMI(X86::CALLpcrel32, 1).addExternalSymbol(FnName[NameIdx], true);

    std::vector<ValueRecord> Args;
    Args.push_back(ValueRecord(Op0Reg, Type::LongTy));
    Args.push_back(ValueRecord(Op1Reg, Type::LongTy));
    doCall(ValueRecord(ResultReg, Type::LongTy), TheCall, Args);
    return;
  }
  case cByte: case cShort: case cInt:
    break;          // Small integerals, handled below...
  default: assert(0 && "Unknown class!");
  }

  static const unsigned Regs[]     ={ X86::AL    , X86::AX     , X86::EAX     };
  static const unsigned MovOpcode[]={ X86::MOVrr8, X86::MOVrr16, X86::MOVrr32 };
  static const unsigned ExtOpcode[]={ X86::CBW   , X86::CWD    , X86::CDQ     };
  static const unsigned ClrOpcode[]={ X86::XORrr8, X86::XORrr16, X86::XORrr32 };
  static const unsigned ExtRegs[]  ={ X86::AH    , X86::DX     , X86::EDX     };

  static const unsigned DivOpcode[][4] = {
    { X86::DIVr8 , X86::DIVr16 , X86::DIVr32 , 0 },  // Unsigned division
    { X86::IDIVr8, X86::IDIVr16, X86::IDIVr32, 0 },  // Signed division
  };

  bool isSigned   = I.getType()->isSigned();
  unsigned Reg    = Regs[Class];
  unsigned ExtReg = ExtRegs[Class];

  // Put the first operand into one of the A registers...
  BuildMI(BB, MovOpcode[Class], 1, Reg).addReg(Op0Reg);

  if (isSigned) {
    // Emit a sign extension instruction...
    BuildMI(BB, ExtOpcode[Class], 0);
  } else {
    // If unsigned, emit a zeroing instruction... (reg = xor reg, reg)
    BuildMI(BB, ClrOpcode[Class], 2, ExtReg).addReg(ExtReg).addReg(ExtReg);
  }

  // Emit the appropriate divide or remainder instruction...
  BuildMI(BB, DivOpcode[isSigned][Class], 1).addReg(Op1Reg);

  // Figure out which register we want to pick the result out of...
  unsigned DestReg = (I.getOpcode() == Instruction::Div) ? Reg : ExtReg;
  
  // Put the result into the destination register...
  BuildMI(BB, MovOpcode[Class], 1, ResultReg).addReg(DestReg);
}


/// Shift instructions: 'shl', 'sar', 'shr' - Some special cases here
/// for constant immediate shift values, and for constant immediate
/// shift values equal to 1. Even the general case is sort of special,
/// because the shift amount has to be in CL, not just any old register.
///
void ISel::visitShiftInst(ShiftInst &I) {
  unsigned SrcReg = getReg(I.getOperand(0));
  unsigned DestReg = getReg(I);
  bool isLeftShift = I.getOpcode() == Instruction::Shl;
  bool isSigned = I.getType()->isSigned();
  unsigned Class = getClass(I.getType());
  
  static const unsigned ConstantOperand[][4] = {
    { X86::SHRir8, X86::SHRir16, X86::SHRir32, X86::SHRDir32 },  // SHR
    { X86::SARir8, X86::SARir16, X86::SARir32, X86::SHRDir32 },  // SAR
    { X86::SHLir8, X86::SHLir16, X86::SHLir32, X86::SHLDir32 },  // SHL
    { X86::SHLir8, X86::SHLir16, X86::SHLir32, X86::SHLDir32 },  // SAL = SHL
  };

  static const unsigned NonConstantOperand[][4] = {
    { X86::SHRrr8, X86::SHRrr16, X86::SHRrr32 },  // SHR
    { X86::SARrr8, X86::SARrr16, X86::SARrr32 },  // SAR
    { X86::SHLrr8, X86::SHLrr16, X86::SHLrr32 },  // SHL
    { X86::SHLrr8, X86::SHLrr16, X86::SHLrr32 },  // SAL = SHL
  };

  // Longs, as usual, are handled specially...
  if (Class == cLong) {
    // If we have a constant shift, we can generate much more efficient code
    // than otherwise...
    //
    if (ConstantUInt *CUI = dyn_cast<ConstantUInt>(I.getOperand(1))) {
      unsigned Amount = CUI->getValue();
      if (Amount < 32) {
	const unsigned *Opc = ConstantOperand[isLeftShift*2+isSigned];
	if (isLeftShift) {
	  BuildMI(BB, Opc[3], 3, 
		  DestReg+1).addReg(SrcReg+1).addReg(SrcReg).addZImm(Amount);
	  BuildMI(BB, Opc[2], 2, DestReg).addReg(SrcReg).addZImm(Amount);
	} else {
	  BuildMI(BB, Opc[3], 3,
		  DestReg).addReg(SrcReg  ).addReg(SrcReg+1).addZImm(Amount);
	  BuildMI(BB, Opc[2], 2, DestReg+1).addReg(SrcReg+1).addZImm(Amount);
	}
      } else {                 // Shifting more than 32 bits
	Amount -= 32;
	if (isLeftShift) {
	  BuildMI(BB, X86::SHLir32, 2,DestReg+1).addReg(SrcReg).addZImm(Amount);
	  BuildMI(BB, X86::MOVir32, 1,DestReg  ).addZImm(0);
	} else {
	  unsigned Opcode = isSigned ? X86::SARir32 : X86::SHRir32;
	  BuildMI(BB, Opcode, 2, DestReg).addReg(SrcReg+1).addZImm(Amount);
	  BuildMI(BB, X86::MOVir32, 1, DestReg+1).addZImm(0);
	}
      }
    } else {
      visitInstruction(I);  // FIXME: Implement long shift by non-constant
    }
    return;
  }

  if (ConstantUInt *CUI = dyn_cast<ConstantUInt>(I.getOperand(1))) {
    // The shift amount is constant, guaranteed to be a ubyte. Get its value.
    assert(CUI->getType() == Type::UByteTy && "Shift amount not a ubyte?");

    const unsigned *Opc = ConstantOperand[isLeftShift*2+isSigned];
    BuildMI(BB, Opc[Class], 2, DestReg).addReg(SrcReg).addZImm(CUI->getValue());
  } else {                  // The shift amount is non-constant.
    BuildMI(BB, X86::MOVrr8, 1, X86::CL).addReg(getReg(I.getOperand(1)));

    const unsigned *Opc = NonConstantOperand[isLeftShift*2+isSigned];
    BuildMI(BB, Opc[Class], 1, DestReg).addReg(SrcReg);
  }
}


/// doFPLoad - This method is used to load an FP value from memory using the
/// current endianness.  NOTE: This method returns a partially constructed load
/// instruction which needs to have the memory source filled in still.
///
MachineInstr *ISel::doFPLoad(MachineBasicBlock *MBB,
			     MachineBasicBlock::iterator &MBBI,
			     const Type *Ty, unsigned DestReg) {
  assert(Ty == Type::FloatTy || Ty == Type::DoubleTy && "Unknown FP type!");
  unsigned LoadOpcode = Ty == Type::FloatTy ? X86::FLDr32 : X86::FLDr64;

  if (TM.getTargetData().isLittleEndian()) // fast path...
    return BMI(MBB, MBBI, LoadOpcode, 4, DestReg);

  // If we are big-endian, start by creating an LEA instruction to represent the
  // address of the memory location to load from...
  //
  unsigned SrcAddrReg = makeAnotherReg(Type::UIntTy);
  MachineInstr *Result = BMI(MBB, MBBI, X86::LEAr32, 5, SrcAddrReg);

  // Allocate a temporary stack slot to transform the value into...
  int FrameIdx = F->getFrameInfo()->CreateStackObject(Ty, TM.getTargetData());

  // Perform the bswaps 32 bits at a time...
  unsigned TmpReg1 = makeAnotherReg(Type::UIntTy);
  unsigned TmpReg2 = makeAnotherReg(Type::UIntTy);
  addDirectMem(BMI(MBB, MBBI, X86::MOVmr32, 4, TmpReg1), SrcAddrReg);
  BMI(MBB, MBBI, X86::BSWAPr32, 1, TmpReg2).addReg(TmpReg1);
  unsigned Offset = (Ty == Type::DoubleTy) << 2;
  addFrameReference(BMI(MBB, MBBI, X86::MOVrm32, 5),
		    FrameIdx, Offset).addReg(TmpReg2);
  
  if (Ty == Type::DoubleTy) {   // Swap the other 32 bits of a double value...
    TmpReg1 = makeAnotherReg(Type::UIntTy);
    TmpReg2 = makeAnotherReg(Type::UIntTy);

    addRegOffset(BMI(MBB, MBBI, X86::MOVmr32, 4, TmpReg1), SrcAddrReg, 4);
    BMI(MBB, MBBI, X86::BSWAPr32, 1, TmpReg2).addReg(TmpReg1);
    unsigned Offset = (Ty == Type::DoubleTy) << 2;
    addFrameReference(BMI(MBB, MBBI, X86::MOVrm32,5), FrameIdx).addReg(TmpReg2);
  }

  // Now we can reload the final byteswapped result into the final destination.
  addFrameReference(BMI(MBB, MBBI, LoadOpcode, 4, DestReg), FrameIdx);
  return Result;
}

/// EmitByteSwap - Byteswap SrcReg into DestReg.
///
void ISel::EmitByteSwap(unsigned DestReg, unsigned SrcReg, unsigned Class) {
  // Emit the byte swap instruction...
  switch (Class) {
  case cByte:
    // No byteswap necessary for 8 bit value...
    BuildMI(BB, X86::MOVrr8, 1, DestReg).addReg(SrcReg);
    break;
  case cInt:
    // Use the 32 bit bswap instruction to do a 32 bit swap...
    BuildMI(BB, X86::BSWAPr32, 1, DestReg).addReg(SrcReg);
    break;
    
  case cShort:
    // For 16 bit we have to use an xchg instruction, because there is no
    // 16-bit bswap.  XCHG is necessarily not in SSA form, so we force things
    // into AX to do the xchg.
    //
    BuildMI(BB, X86::MOVrr16, 1, X86::AX).addReg(SrcReg);
    BuildMI(BB, X86::XCHGrr8, 2).addReg(X86::AL, MOTy::UseAndDef)
      .addReg(X86::AH, MOTy::UseAndDef);
    BuildMI(BB, X86::MOVrr16, 1, DestReg).addReg(X86::AX);
    break;
  default: assert(0 && "Cannot byteswap this class!");
  }
}


/// visitLoadInst - Implement LLVM load instructions in terms of the x86 'mov'
/// instruction.  The load and store instructions are the only place where we
/// need to worry about the memory layout of the target machine.
///
void ISel::visitLoadInst(LoadInst &I) {
  bool isLittleEndian  = TM.getTargetData().isLittleEndian();
  bool hasLongPointers = TM.getTargetData().getPointerSize() == 8;
  unsigned SrcAddrReg = getReg(I.getOperand(0));
  unsigned DestReg = getReg(I);

  unsigned Class = getClass(I.getType());
  switch (Class) {
  case cFP: {
    MachineBasicBlock::iterator MBBI = BB->end();
    addDirectMem(doFPLoad(BB, MBBI, I.getType(), DestReg), SrcAddrReg);
    return;
  }
  case cLong: case cInt: case cShort: case cByte:
    break;      // Integers of various sizes handled below
  default: assert(0 && "Unknown memory class!");
  }

  // We need to adjust the input pointer if we are emulating a big-endian
  // long-pointer target.  On these systems, the pointer that we are interested
  // in is in the upper part of the eight byte memory image of the pointer.  It
  // also happens to be byte-swapped, but this will be handled later.
  //
  if (!isLittleEndian && hasLongPointers && isa<PointerType>(I.getType())) {
    unsigned R = makeAnotherReg(Type::UIntTy);
    BuildMI(BB, X86::ADDri32, 2, R).addReg(SrcAddrReg).addZImm(4);
    SrcAddrReg = R;
  }

  unsigned IReg = DestReg;
  if (!isLittleEndian)  // If big endian we need an intermediate stage
    DestReg = makeAnotherReg(Class != cLong ? I.getType() : Type::UIntTy);

  static const unsigned Opcode[] = {
    X86::MOVmr8, X86::MOVmr16, X86::MOVmr32, 0, X86::MOVmr32
  };
  addDirectMem(BuildMI(BB, Opcode[Class], 4, DestReg), SrcAddrReg);

  // Handle long values now...
  if (Class == cLong) {
    if (isLittleEndian) {
      addRegOffset(BuildMI(BB, X86::MOVmr32, 4, DestReg+1), SrcAddrReg, 4);
    } else {
      EmitByteSwap(IReg+1, DestReg, cInt);
      unsigned TempReg = makeAnotherReg(Type::IntTy);
      addRegOffset(BuildMI(BB, X86::MOVmr32, 4, TempReg), SrcAddrReg, 4);
      EmitByteSwap(IReg, TempReg, cInt);
    }
    return;
  }

  if (!isLittleEndian)
    EmitByteSwap(IReg, DestReg, Class);
}


/// doFPStore - This method is used to store an FP value to memory using the
/// current endianness.
///
void ISel::doFPStore(const Type *Ty, unsigned DestAddrReg, unsigned SrcReg) {
  assert(Ty == Type::FloatTy || Ty == Type::DoubleTy && "Unknown FP type!");
  unsigned StoreOpcode = Ty == Type::FloatTy ? X86::FSTr32 : X86::FSTr64;

  if (TM.getTargetData().isLittleEndian()) {  // fast path...
    addDirectMem(BuildMI(BB, StoreOpcode,5), DestAddrReg).addReg(SrcReg);
    return;
  }

  // Allocate a temporary stack slot to transform the value into...
  int FrameIdx = F->getFrameInfo()->CreateStackObject(Ty, TM.getTargetData());
  unsigned SrcAddrReg = makeAnotherReg(Type::UIntTy);
  addFrameReference(BuildMI(BB, X86::LEAr32, 5, SrcAddrReg), FrameIdx);

  // Store the value into a temporary stack slot...
  addDirectMem(BuildMI(BB, StoreOpcode, 5), SrcAddrReg).addReg(SrcReg);

  // Perform the bswaps 32 bits at a time...
  unsigned TmpReg1 = makeAnotherReg(Type::UIntTy);
  unsigned TmpReg2 = makeAnotherReg(Type::UIntTy);
  addDirectMem(BuildMI(BB, X86::MOVmr32, 4, TmpReg1), SrcAddrReg);
  BuildMI(BB, X86::BSWAPr32, 1, TmpReg2).addReg(TmpReg1);
  unsigned Offset = (Ty == Type::DoubleTy) << 2;
  addRegOffset(BuildMI(BB, X86::MOVrm32, 5),
	       DestAddrReg, Offset).addReg(TmpReg2);
  
  if (Ty == Type::DoubleTy) {   // Swap the other 32 bits of a double value...
    TmpReg1 = makeAnotherReg(Type::UIntTy);
    TmpReg2 = makeAnotherReg(Type::UIntTy);

    addRegOffset(BuildMI(BB, X86::MOVmr32, 4, TmpReg1), SrcAddrReg, 4);
    BuildMI(BB, X86::BSWAPr32, 1, TmpReg2).addReg(TmpReg1);
    unsigned Offset = (Ty == Type::DoubleTy) << 2;
    addDirectMem(BuildMI(BB, X86::MOVrm32, 5), DestAddrReg).addReg(TmpReg2);
  }
}


/// visitStoreInst - Implement LLVM store instructions in terms of the x86 'mov'
/// instruction.
///
void ISel::visitStoreInst(StoreInst &I) {
  bool isLittleEndian  = TM.getTargetData().isLittleEndian();
  bool hasLongPointers = TM.getTargetData().getPointerSize() == 8;
  unsigned ValReg      = getReg(I.getOperand(0));
  unsigned AddressReg  = getReg(I.getOperand(1));

  unsigned Class = getClass(I.getOperand(0)->getType());
  switch (Class) {
  case cLong:
    if (isLittleEndian) {
      addDirectMem(BuildMI(BB, X86::MOVrm32, 1+4), AddressReg).addReg(ValReg);
      addRegOffset(BuildMI(BB, X86::MOVrm32, 1+4),
		   AddressReg, 4).addReg(ValReg+1);
    } else {
      unsigned T1 = makeAnotherReg(Type::IntTy);
      unsigned T2 = makeAnotherReg(Type::IntTy);
      EmitByteSwap(T1, ValReg  , cInt);
      EmitByteSwap(T2, ValReg+1, cInt);
      addDirectMem(BuildMI(BB, X86::MOVrm32, 1+4), AddressReg).addReg(T2);
      addRegOffset(BuildMI(BB, X86::MOVrm32, 1+4), AddressReg, 4).addReg(T1);
    }
    return;
  case cFP:
    doFPStore(I.getOperand(0)->getType(), AddressReg, ValReg);
    return;
  case cInt: case cShort: case cByte:
    break;      // Integers of various sizes handled below
  default: assert(0 && "Unknown memory class!");
  }

  if (!isLittleEndian && hasLongPointers &&
      isa<PointerType>(I.getOperand(0)->getType())) {
    unsigned R = makeAnotherReg(Type::UIntTy);
    BuildMI(BB, X86::ADDri32, 2, R).addReg(AddressReg).addZImm(4);
    AddressReg = R;
  }

  if (!isLittleEndian && Class != cByte) {
    unsigned R = makeAnotherReg(I.getOperand(0)->getType());
    EmitByteSwap(R, ValReg, Class);
    ValReg = R;
  }

  static const unsigned Opcode[] = { X86::MOVrm8, X86::MOVrm16, X86::MOVrm32 };
  addDirectMem(BuildMI(BB, Opcode[Class], 1+4), AddressReg).addReg(ValReg);
}


/// visitCastInst - Here we have various kinds of copying with or without
/// sign extension going on.
void ISel::visitCastInst(CastInst &CI) {
  unsigned DestReg = getReg(CI);
  MachineBasicBlock::iterator MI = BB->end();
  emitCastOperation(BB, MI, CI.getOperand(0), CI.getType(), DestReg);
}

/// emitCastOperation - Common code shared between visitCastInst and
/// constant expression cast support.
void ISel::emitCastOperation(MachineBasicBlock *BB,
                             MachineBasicBlock::iterator &IP,
                             Value *Src, const Type *DestTy,
                             unsigned DestReg) {
  unsigned SrcReg = getReg(Src, BB, IP);
  const Type *SrcTy = Src->getType();
  unsigned SrcClass = getClassB(SrcTy);
  unsigned DestClass = getClassB(DestTy);

  // Implement casts to bool by using compare on the operand followed by set if
  // not zero on the result.
  if (DestTy == Type::BoolTy) {
    if (SrcClass == cFP || SrcClass == cLong)
      abort();  // FIXME: implement cast (long & FP) to bool
    
    BMI(BB, IP, X86::CMPri8, 2).addReg(SrcReg).addZImm(0);
    BMI(BB, IP, X86::SETNEr, 1, DestReg);
    return;
  }

  static const unsigned RegRegMove[] = {
    X86::MOVrr8, X86::MOVrr16, X86::MOVrr32, X86::FpMOV, X86::MOVrr32
  };

  // Implement casts between values of the same type class (as determined by
  // getClass) by using a register-to-register move.
  if (SrcClass == DestClass) {
    if (SrcClass <= cInt || (SrcClass == cFP && SrcTy == DestTy)) {
      BMI(BB, IP, RegRegMove[SrcClass], 1, DestReg).addReg(SrcReg);
    } else if (SrcClass == cFP) {
      if (SrcTy == Type::FloatTy) {  // double -> float
	assert(DestTy == Type::DoubleTy && "Unknown cFP member!");
	BMI(BB, IP, X86::FpMOV, 1, DestReg).addReg(SrcReg);
      } else {                       // float -> double
	assert(SrcTy == Type::DoubleTy && DestTy == Type::FloatTy &&
	       "Unknown cFP member!");
	// Truncate from double to float by storing to memory as short, then
	// reading it back.
	unsigned FltAlign = TM.getTargetData().getFloatAlignment();
        int FrameIdx = F->getFrameInfo()->CreateStackObject(4, FltAlign);
	addFrameReference(BMI(BB, IP, X86::FSTr32, 5), FrameIdx).addReg(SrcReg);
	addFrameReference(BMI(BB, IP, X86::FLDr32, 5, DestReg), FrameIdx);
      }
    } else if (SrcClass == cLong) {
      BMI(BB, IP, X86::MOVrr32, 1, DestReg).addReg(SrcReg);
      BMI(BB, IP, X86::MOVrr32, 1, DestReg+1).addReg(SrcReg+1);
    } else {
      abort();
    }
    return;
  }

  // Handle cast of SMALLER int to LARGER int using a move with sign extension
  // or zero extension, depending on whether the source type was signed.
  if (SrcClass <= cInt && (DestClass <= cInt || DestClass == cLong) &&
      SrcClass < DestClass) {
    bool isLong = DestClass == cLong;
    if (isLong) DestClass = cInt;

    static const unsigned Opc[][4] = {
      { X86::MOVSXr16r8, X86::MOVSXr32r8, X86::MOVSXr32r16, X86::MOVrr32 }, // s
      { X86::MOVZXr16r8, X86::MOVZXr32r8, X86::MOVZXr32r16, X86::MOVrr32 }  // u
    };
    
    bool isUnsigned = SrcTy->isUnsigned();
    BMI(BB, IP, Opc[isUnsigned][SrcClass + DestClass - 1], 1,
        DestReg).addReg(SrcReg);

    if (isLong) {  // Handle upper 32 bits as appropriate...
      if (isUnsigned)     // Zero out top bits...
	BMI(BB, IP, X86::MOVir32, 1, DestReg+1).addZImm(0);
      else                // Sign extend bottom half...
	BMI(BB, IP, X86::SARir32, 2, DestReg+1).addReg(DestReg).addZImm(31);
    }
    return;
  }

  // Special case long -> int ...
  if (SrcClass == cLong && DestClass == cInt) {
    BMI(BB, IP, X86::MOVrr32, 1, DestReg).addReg(SrcReg);
    return;
  }
  
  // Handle cast of LARGER int to SMALLER int using a move to EAX followed by a
  // move out of AX or AL.
  if ((SrcClass <= cInt || SrcClass == cLong) && DestClass <= cInt
      && SrcClass > DestClass) {
    static const unsigned AReg[] = { X86::AL, X86::AX, X86::EAX, 0, X86::EAX };
    BMI(BB, IP, RegRegMove[SrcClass], 1, AReg[SrcClass]).addReg(SrcReg);
    BMI(BB, IP, RegRegMove[DestClass], 1, DestReg).addReg(AReg[DestClass]);
    return;
  }

  // Handle casts from integer to floating point now...
  if (DestClass == cFP) {
    // unsigned int -> load as 64 bit int.
    // unsigned long long -> more complex
    if (SrcTy->isUnsigned() && SrcTy != Type::UByteTy)
      abort();  // don't handle unsigned src yet!

    // We don't have the facilities for directly loading byte sized data from
    // memory.  Promote it to 16 bits.
    if (SrcClass == cByte) {
      unsigned TmpReg = makeAnotherReg(Type::ShortTy);
      BMI(BB, IP, SrcTy->isSigned() ? X86::MOVSXr16r8 : X86::MOVZXr16r8,
          1, TmpReg).addReg(SrcReg);
      SrcTy = Type::ShortTy;     // Pretend the short is our input now!
      SrcClass = cShort;
      SrcReg = TmpReg;
    }

    // Spill the integer to memory and reload it from there...
    int FrameIdx =
      F->getFrameInfo()->CreateStackObject(SrcTy, TM.getTargetData());

    if (SrcClass == cLong) {
      if (SrcTy == Type::ULongTy) abort();  // FIXME: Handle ulong -> FP
      addFrameReference(BMI(BB, IP, X86::MOVrm32, 5), FrameIdx).addReg(SrcReg);
      addFrameReference(BMI(BB, IP, X86::MOVrm32, 5),
			FrameIdx, 4).addReg(SrcReg+1);
    } else {
      static const unsigned Op1[] = { X86::MOVrm8, X86::MOVrm16, X86::MOVrm32 };
      addFrameReference(BMI(BB, IP, Op1[SrcClass], 5), FrameIdx).addReg(SrcReg);
    }

    static const unsigned Op2[] =
      { 0, X86::FILDr16, X86::FILDr32, 0, X86::FILDr64 };
    addFrameReference(BMI(BB, IP, Op2[SrcClass], 5, DestReg), FrameIdx);
    return;
  }

  // Handle casts from floating point to integer now...
  if (SrcClass == cFP) {
    // Change the floating point control register to use "round towards zero"
    // mode when truncating to an integer value.
    //
    int CWFrameIdx = F->getFrameInfo()->CreateStackObject(2, 2);
    addFrameReference(BMI(BB, IP, X86::FNSTCWm16, 4), CWFrameIdx);

    // Load the old value of the high byte of the control word...
    unsigned HighPartOfCW = makeAnotherReg(Type::UByteTy);
    addFrameReference(BMI(BB, IP, X86::MOVmr8, 4, HighPartOfCW), CWFrameIdx, 1);

    // Set the high part to be round to zero...
    addFrameReference(BMI(BB, IP, X86::MOVim8, 5), CWFrameIdx, 1).addZImm(12);

    // Reload the modified control word now...
    addFrameReference(BMI(BB, IP, X86::FLDCWm16, 4), CWFrameIdx);
    
    // Restore the memory image of control word to original value
    addFrameReference(BMI(BB, IP, X86::MOVrm8, 5),
		      CWFrameIdx, 1).addReg(HighPartOfCW);

    // We don't have the facilities for directly storing byte sized data to
    // memory.  Promote it to 16 bits.  We also must promote unsigned values to
    // larger classes because we only have signed FP stores.
    unsigned StoreClass  = DestClass;
    const Type *StoreTy  = DestTy;
    if (StoreClass == cByte || DestTy->isUnsigned())
      switch (StoreClass) {
      case cByte:  StoreTy = Type::ShortTy; StoreClass = cShort; break;
      case cShort: StoreTy = Type::IntTy;   StoreClass = cInt;   break;
      case cInt:   StoreTy = Type::LongTy;  StoreClass = cLong;  break;
      case cLong:  abort(); // FIXME: unsigned long long -> more complex
      default: assert(0 && "Unknown store class!");
      }

    // Spill the integer to memory and reload it from there...
    int FrameIdx =
      F->getFrameInfo()->CreateStackObject(StoreTy, TM.getTargetData());

    static const unsigned Op1[] =
      { 0, X86::FISTr16, X86::FISTr32, 0, X86::FISTPr64 };
    addFrameReference(BMI(BB, IP, Op1[StoreClass], 5), FrameIdx).addReg(SrcReg);

    if (DestClass == cLong) {
      addFrameReference(BMI(BB, IP, X86::MOVmr32, 4, DestReg), FrameIdx);
      addFrameReference(BMI(BB, IP, X86::MOVmr32, 4, DestReg+1), FrameIdx, 4);
    } else {
      static const unsigned Op2[] = { X86::MOVmr8, X86::MOVmr16, X86::MOVmr32 };
      addFrameReference(BMI(BB, IP, Op2[DestClass], 4, DestReg), FrameIdx);
    }

    // Reload the original control word now...
    addFrameReference(BMI(BB, IP, X86::FLDCWm16, 4), CWFrameIdx);
    return;
  }

  // Anything we haven't handled already, we can't (yet) handle at all.
  abort();
}

// ExactLog2 - This function solves for (Val == 1 << (N-1)) and returns N.  It
// returns zero when the input is not exactly a power of two.
static unsigned ExactLog2(unsigned Val) {
  if (Val == 0) return 0;
  unsigned Count = 0;
  while (Val != 1) {
    if (Val & 1) return 0;
    Val >>= 1;
    ++Count;
  }
  return Count+1;
}

void ISel::visitGetElementPtrInst(GetElementPtrInst &I) {
  unsigned outputReg = getReg(I);
  MachineBasicBlock::iterator MI = BB->end();
  emitGEPOperation(BB, MI, I.getOperand(0),
                   I.op_begin()+1, I.op_end(), outputReg);
}

void ISel::emitGEPOperation(MachineBasicBlock *MBB,
                            MachineBasicBlock::iterator &IP,
                            Value *Src, User::op_iterator IdxBegin,
                            User::op_iterator IdxEnd, unsigned TargetReg) {
  const TargetData &TD = TM.getTargetData();
  const Type *Ty = Src->getType();
  unsigned BaseReg = getReg(Src, MBB, IP);

  // GEPs have zero or more indices; we must perform a struct access
  // or array access for each one.
  for (GetElementPtrInst::op_iterator oi = IdxBegin,
         oe = IdxEnd; oi != oe; ++oi) {
    Value *idx = *oi;
    unsigned NextReg = BaseReg;
    if (const StructType *StTy = dyn_cast<StructType>(Ty)) {
      // It's a struct access.  idx is the index into the structure,
      // which names the field. This index must have ubyte type.
      const ConstantUInt *CUI = cast<ConstantUInt>(idx);
      assert(CUI->getType() == Type::UByteTy
	      && "Funny-looking structure index in GEP");
      // Use the TargetData structure to pick out what the layout of
      // the structure is in memory.  Since the structure index must
      // be constant, we can get its value and use it to find the
      // right byte offset from the StructLayout class's list of
      // structure member offsets.
      unsigned idxValue = CUI->getValue();
      unsigned FieldOff = TD.getStructLayout(StTy)->MemberOffsets[idxValue];
      if (FieldOff) {
	NextReg = makeAnotherReg(Type::UIntTy);
	// Emit an ADD to add FieldOff to the basePtr.
	BMI(MBB, IP, X86::ADDri32, 2,NextReg).addReg(BaseReg).addZImm(FieldOff);
      }
      // The next type is the member of the structure selected by the
      // index.
      Ty = StTy->getElementTypes()[idxValue];
    } else if (const SequentialType *SqTy = cast<SequentialType>(Ty)) {
      // It's an array or pointer access: [ArraySize x ElementType].

      // idx is the index into the array.  Unlike with structure
      // indices, we may not know its actual value at code-generation
      // time.
      assert(idx->getType() == Type::LongTy && "Bad GEP array index!");

      // We want to add BaseReg to(idxReg * sizeof ElementType). First, we
      // must find the size of the pointed-to type (Not coincidentally, the next
      // type is the type of the elements in the array).
      Ty = SqTy->getElementType();
      unsigned elementSize = TD.getTypeSize(Ty);

      // If idxReg is a constant, we don't need to perform the multiply!
      if (ConstantSInt *CSI = dyn_cast<ConstantSInt>(idx)) {
        if (!CSI->isNullValue()) {
          unsigned Offset = elementSize*CSI->getValue();
	  NextReg = makeAnotherReg(Type::UIntTy);
          BMI(MBB, IP, X86::ADDri32, 2,NextReg).addReg(BaseReg).addZImm(Offset);
        }
      } else if (elementSize == 1) {
        // If the element size is 1, we don't have to multiply, just add
        unsigned idxReg = getReg(idx, MBB, IP);
	NextReg = makeAnotherReg(Type::UIntTy);
        BMI(MBB, IP, X86::ADDrr32, 2, NextReg).addReg(BaseReg).addReg(idxReg);
      } else {
        unsigned idxReg = getReg(idx, MBB, IP);
        unsigned OffsetReg = makeAnotherReg(Type::UIntTy);
        if (unsigned Shift = ExactLog2(elementSize)) {
          // If the element size is exactly a power of 2, use a shift to get it.
          BMI(MBB, IP, X86::SHLir32, 2,
              OffsetReg).addReg(idxReg).addZImm(Shift-1);
        } else {
          // Most general case, emit a multiply...
          unsigned elementSizeReg = makeAnotherReg(Type::LongTy);
          BMI(MBB, IP, X86::MOVir32, 1, elementSizeReg).addZImm(elementSize);
        
          // Emit a MUL to multiply the register holding the index by
          // elementSize, putting the result in OffsetReg.
          doMultiply(MBB, IP, OffsetReg, Type::IntTy, idxReg, elementSizeReg);
        }
        // Emit an ADD to add OffsetReg to the basePtr.
	NextReg = makeAnotherReg(Type::UIntTy);
        BMI(MBB, IP, X86::ADDrr32, 2,NextReg).addReg(BaseReg).addReg(OffsetReg);
      }
    }
    // Now that we are here, further indices refer to subtypes of this
    // one, so we don't need to worry about BaseReg itself, anymore.
    BaseReg = NextReg;
  }
  // After we have processed all the indices, the result is left in
  // BaseReg.  Move it to the register where we were expected to
  // put the answer.  A 32-bit move should do it, because we are in
  // ILP32 land.
  BMI(MBB, IP, X86::MOVrr32, 1, TargetReg).addReg(BaseReg);
}


/// visitAllocaInst - If this is a fixed size alloca, allocate space from the
/// frame manager, otherwise do it the hard way.
///
void ISel::visitAllocaInst(AllocaInst &I) {
  // Find the data size of the alloca inst's getAllocatedType.
  const Type *Ty = I.getAllocatedType();
  unsigned TySize = TM.getTargetData().getTypeSize(Ty);

  // If this is a fixed size alloca in the entry block for the function,
  // statically stack allocate the space.
  //
  if (ConstantUInt *CUI = dyn_cast<ConstantUInt>(I.getArraySize())) {
    if (I.getParent() == I.getParent()->getParent()->begin()) {
      TySize *= CUI->getValue();   // Get total allocated size...
      unsigned Alignment = TM.getTargetData().getTypeAlignment(Ty);
      
      // Create a new stack object using the frame manager...
      int FrameIdx = F->getFrameInfo()->CreateStackObject(TySize, Alignment);
      addFrameReference(BuildMI(BB, X86::LEAr32, 5, getReg(I)), FrameIdx);
      return;
    }
  }
  
  // Create a register to hold the temporary result of multiplying the type size
  // constant by the variable amount.
  unsigned TotalSizeReg = makeAnotherReg(Type::UIntTy);
  unsigned SrcReg1 = getReg(I.getArraySize());
  unsigned SizeReg = makeAnotherReg(Type::UIntTy);
  BuildMI(BB, X86::MOVir32, 1, SizeReg).addZImm(TySize);
  
  // TotalSizeReg = mul <numelements>, <TypeSize>
  MachineBasicBlock::iterator MBBI = BB->end();
  doMultiply(BB, MBBI, TotalSizeReg, Type::UIntTy, SrcReg1, SizeReg);

  // AddedSize = add <TotalSizeReg>, 15
  unsigned AddedSizeReg = makeAnotherReg(Type::UIntTy);
  BuildMI(BB, X86::ADDri32, 2, AddedSizeReg).addReg(TotalSizeReg).addZImm(15);

  // AlignedSize = and <AddedSize>, ~15
  unsigned AlignedSize = makeAnotherReg(Type::UIntTy);
  BuildMI(BB, X86::ANDri32, 2, AlignedSize).addReg(AddedSizeReg).addZImm(~15);
  
  // Subtract size from stack pointer, thereby allocating some space.
  BuildMI(BB, X86::SUBrr32, 2, X86::ESP).addReg(X86::ESP).addReg(AlignedSize);

  // Put a pointer to the space into the result register, by copying
  // the stack pointer.
  BuildMI(BB, X86::MOVrr32, 1, getReg(I)).addReg(X86::ESP);

  // Inform the Frame Information that we have just allocated a variable-sized
  // object.
  F->getFrameInfo()->CreateVariableSizedObject();
}

/// visitMallocInst - Malloc instructions are code generated into direct calls
/// to the library malloc.
///
void ISel::visitMallocInst(MallocInst &I) {
  unsigned AllocSize = TM.getTargetData().getTypeSize(I.getAllocatedType());
  unsigned Arg;

  if (ConstantUInt *C = dyn_cast<ConstantUInt>(I.getOperand(0))) {
    Arg = getReg(ConstantUInt::get(Type::UIntTy, C->getValue() * AllocSize));
  } else {
    Arg = makeAnotherReg(Type::UIntTy);
    unsigned Op0Reg = getReg(ConstantUInt::get(Type::UIntTy, AllocSize));
    unsigned Op1Reg = getReg(I.getOperand(0));
    MachineBasicBlock::iterator MBBI = BB->end();
    doMultiply(BB, MBBI, Arg, Type::UIntTy, Op0Reg, Op1Reg);
	       
	       
  }

  std::vector<ValueRecord> Args;
  Args.push_back(ValueRecord(Arg, Type::UIntTy));
  MachineInstr *TheCall = BuildMI(X86::CALLpcrel32,
				  1).addExternalSymbol("malloc", true);
  doCall(ValueRecord(getReg(I), I.getType()), TheCall, Args);
}


/// visitFreeInst - Free instructions are code gen'd to call the free libc
/// function.
///
void ISel::visitFreeInst(FreeInst &I) {
  std::vector<ValueRecord> Args;
  Args.push_back(ValueRecord(getReg(I.getOperand(0)),
			     I.getOperand(0)->getType()));
  MachineInstr *TheCall = BuildMI(X86::CALLpcrel32,
				  1).addExternalSymbol("free", true);
  doCall(ValueRecord(0, Type::VoidTy), TheCall, Args);
}
   

/// createSimpleX86InstructionSelector - This pass converts an LLVM function
/// into a machine code representation is a very simple peep-hole fashion.  The
/// generated code sucks but the implementation is nice and simple.
///
Pass *createSimpleX86InstructionSelector(TargetMachine &TM) {
  return new ISel(TM);
}
