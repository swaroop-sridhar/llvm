//===- MachineCopyPropagation.cpp - Machine Copy Propagation Pass ---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This is an extremely simple MachineInstr-level copy propagation pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/CodeGen/Passes.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
using namespace llvm;

#define DEBUG_TYPE "codegen-cp"

STATISTIC(NumDeletes, "Number of dead copies deleted");

namespace {
  class MachineCopyPropagation : public MachineFunctionPass {
    const TargetRegisterInfo *TRI;
    const TargetInstrInfo *TII;
    const MachineRegisterInfo *MRI;

  public:
    static char ID; // Pass identification, replacement for typeid
    MachineCopyPropagation() : MachineFunctionPass(ID) {
      initializeMachineCopyPropagationPass(*PassRegistry::getPassRegistry());
    }

    bool runOnMachineFunction(MachineFunction &MF) override;

  private:
    typedef SmallVector<unsigned, 4> DestList;
    typedef DenseMap<unsigned, DestList> SourceMap;
    typedef DenseMap<unsigned, MachineInstr*> Reg2MIMap;

    void SourceNoLongerAvailable(unsigned Reg);
    void CopyPropagateBlock(MachineBasicBlock &MBB);

    /// Candidates for deletion.
    SmallSetVector<MachineInstr*, 8> MaybeDeadCopies;
    /// Def -> available copies map.
    Reg2MIMap AvailCopyMap;
    /// Def -> copies map.
    Reg2MIMap CopyMap;
    /// Src -> Def map
    SourceMap SrcMap;
    bool Changed;
  };
}
char MachineCopyPropagation::ID = 0;
char &llvm::MachineCopyPropagationID = MachineCopyPropagation::ID;

INITIALIZE_PASS(MachineCopyPropagation, "machine-cp",
                "Machine Copy Propagation Pass", false, false)

void MachineCopyPropagation::SourceNoLongerAvailable(unsigned Reg) {
  for (MCRegAliasIterator AI(Reg, TRI, true); AI.isValid(); ++AI) {
    SourceMap::iterator SI = SrcMap.find(*AI);
    if (SI != SrcMap.end()) {
      const DestList& Defs = SI->second;
      for (unsigned MappedDef : Defs) {
        // Source of copy is no longer available for propagation.
        for (MCSubRegIterator SR(MappedDef, TRI, true); SR.isValid(); ++SR)
          AvailCopyMap.erase(*SR);
      }
    }
  }
}

static bool NoInterveningSideEffect(const MachineInstr *CopyMI,
                                    const MachineInstr *MI) {
  const MachineBasicBlock *MBB = CopyMI->getParent();
  if (MI->getParent() != MBB)
    return false;

  for (MachineBasicBlock::const_instr_iterator
           I = std::next(CopyMI->getInstrIterator()),
           E = MBB->instr_end(), E2 = MI->getInstrIterator();
       I != E && I != E2; ++I) {
    if (I->hasUnmodeledSideEffects() || I->isCall() ||
        I->isTerminator())
      return false;
  }
  return true;
}

/// isNopCopy - Return true if the specified copy is really a nop. That is
/// if the source of the copy is the same of the definition of the copy that
/// supplied the source. If the source of the copy is a sub-register than it
/// must check the sub-indices match. e.g.
/// ecx = mov eax
/// al  = mov cl
/// But not
/// ecx = mov eax
/// al  = mov ch
static bool isNopCopy(const MachineInstr *CopyMI, unsigned Def, unsigned Src,
                      const TargetRegisterInfo *TRI) {
  unsigned SrcSrc = CopyMI->getOperand(1).getReg();
  if (Def == SrcSrc)
    return true;
  if (TRI->isSubRegister(SrcSrc, Def)) {
    unsigned SrcDef = CopyMI->getOperand(0).getReg();
    unsigned SubIdx = TRI->getSubRegIndex(SrcSrc, Def);
    if (!SubIdx)
      return false;
    return SubIdx == TRI->getSubRegIndex(SrcDef, Src);
  }

  return false;
}

void MachineCopyPropagation::CopyPropagateBlock(MachineBasicBlock &MBB) {
  DEBUG(dbgs() << "MCP: CopyPropagateBlock " << MBB.getName() << "\n");

  for (MachineBasicBlock::iterator I = MBB.begin(), E = MBB.end(); I != E; ) {
    MachineInstr *MI = &*I;
    ++I;

    if (MI->isCopy()) {
      unsigned Def = MI->getOperand(0).getReg();
      unsigned Src = MI->getOperand(1).getReg();

      assert(!TargetRegisterInfo::isVirtualRegister(Def) &&
             !TargetRegisterInfo::isVirtualRegister(Src) &&
             "MachineCopyPropagation should be run after register allocation!");

      DenseMap<unsigned, MachineInstr*>::iterator CI = AvailCopyMap.find(Src);
      if (CI != AvailCopyMap.end()) {
        MachineInstr *CopyMI = CI->second;
        if (!MRI->isReserved(Def) &&
            (!MRI->isReserved(Src) || NoInterveningSideEffect(CopyMI, MI)) &&
            isNopCopy(CopyMI, Def, Src, TRI)) {
          // The two copies cancel out and the source of the first copy
          // hasn't been overridden, eliminate the second one. e.g.
          //  %ECX<def> = COPY %EAX<kill>
          //  ... nothing clobbered EAX.
          //  %EAX<def> = COPY %ECX
          // =>
          //  %ECX<def> = COPY %EAX
          //
          // Also avoid eliminating a copy from reserved registers unless the
          // definition is proven not clobbered. e.g.
          // %RSP<def> = COPY %RAX
          // CALL
          // %RAX<def> = COPY %RSP

          DEBUG(dbgs() << "MCP: copy is a NOP, removing: "; MI->dump());

          // Clear any kills of Def between CopyMI and MI. This extends the
          // live range.
          for (MachineInstr &MMI :
               make_range(CopyMI->getInstrIterator(), MI->getInstrIterator()))
            MMI.clearRegisterKills(Def, TRI);

          MI->eraseFromParent();
          Changed = true;
          ++NumDeletes;
          continue;
        }
      }

      // If Src is defined by a previous copy, the previous copy cannot be
      // eliminated.
      for (MCRegAliasIterator AI(Src, TRI, true); AI.isValid(); ++AI) {
        Reg2MIMap::iterator CI = CopyMap.find(*AI);
        if (CI != CopyMap.end()) {
          DEBUG(dbgs() << "MCP: Copy is no longer dead: "; CI->second->dump());
          MaybeDeadCopies.remove(CI->second);
        }
      }

      DEBUG(dbgs() << "MCP: Copy is a deletion candidate: "; MI->dump());

      // Copy is now a candidate for deletion.
      if (!MRI->isReserved(Def))
        MaybeDeadCopies.insert(MI);

      // If 'Def' is previously source of another copy, then this earlier copy's
      // source is no longer available. e.g.
      // %xmm9<def> = copy %xmm2
      // ...
      // %xmm2<def> = copy %xmm0
      // ...
      // %xmm2<def> = copy %xmm9
      SourceNoLongerAvailable(Def);

      // Remember Def is defined by the copy.
      // ... Make sure to clear the def maps of aliases first.
      for (MCRegAliasIterator AI(Def, TRI, false); AI.isValid(); ++AI) {
        CopyMap.erase(*AI);
        AvailCopyMap.erase(*AI);
      }
      for (MCSubRegIterator SR(Def, TRI, /*IncludeSelf=*/true); SR.isValid();
           ++SR) {
        CopyMap[*SR] = MI;
        AvailCopyMap[*SR] = MI;
      }

      // Remember source that's copied to Def. Once it's clobbered, then
      // it's no longer available for copy propagation.
      SmallVectorImpl<unsigned> &DestList = SrcMap[Src];
      if (std::find(DestList.begin(), DestList.end(), Def) == DestList.end())
        DestList.push_back(Def);

      continue;
    }

    // Not a copy.
    SmallVector<unsigned, 2> Defs;
    const MachineOperand *RegMask = nullptr;
    for (const MachineOperand &MO : MI->operands()) {
      if (MO.isRegMask())
        RegMask = &MO;
      if (!MO.isReg())
        continue;
      unsigned Reg = MO.getReg();
      if (!Reg)
        continue;

      assert(!TargetRegisterInfo::isVirtualRegister(Reg) &&
             "MachineCopyPropagation should be run after register allocation!");

      if (MO.isDef()) {
        Defs.push_back(Reg);
        continue;
      }

      // If 'Reg' is defined by a copy, the copy is no longer a candidate
      // for elimination.
      for (MCRegAliasIterator AI(Reg, TRI, true); AI.isValid(); ++AI) {
        Reg2MIMap::iterator CI = CopyMap.find(*AI);
        if (CI != CopyMap.end()) {
          DEBUG(dbgs() << "MCP: Copy is used - not dead: "; CI->second->dump());
          MaybeDeadCopies.remove(CI->second);
        }
      }
      // Treat undef use like defs for copy propagation but not for
      // dead copy. We would need to do a liveness check to be sure the copy
      // is dead for undef uses.
      // The backends are allowed to do whatever they want with undef value
      // and we cannot be sure this register will not be rewritten to break
      // some false dependencies for the hardware for instance.
      if (MO.isUndef())
        Defs.push_back(Reg);
    }

    // The instruction has a register mask operand which means that it clobbers
    // a large set of registers.  It is possible to use the register mask to
    // prune the available copies, but treat it like a basic block boundary for
    // now.
    if (RegMask) {
      // Erase any MaybeDeadCopies whose destination register is clobbered.
      for (MachineInstr *MaybeDead : MaybeDeadCopies) {
        unsigned Reg = MaybeDead->getOperand(0).getReg();
        assert(!MRI->isReserved(Reg));
        if (!RegMask->clobbersPhysReg(Reg))
          continue;
        DEBUG(dbgs() << "MCP: Removing copy due to regmask clobbering: ";
              MaybeDead->dump());
        MaybeDead->eraseFromParent();
        Changed = true;
        ++NumDeletes;
      }

      // Clear all data structures as if we were beginning a new basic block.
      MaybeDeadCopies.clear();
      AvailCopyMap.clear();
      CopyMap.clear();
      SrcMap.clear();
      continue;
    }

    for (unsigned Reg : Defs) {
      // No longer defined by a copy.
      for (MCRegAliasIterator AI(Reg, TRI, true); AI.isValid(); ++AI) {
        CopyMap.erase(*AI);
        AvailCopyMap.erase(*AI);
      }

      // If 'Reg' is previously source of a copy, it is no longer available for
      // copy propagation.
      SourceNoLongerAvailable(Reg);
    }
  }

  // If MBB doesn't have successors, delete the copies whose defs are not used.
  // If MBB does have successors, then conservative assume the defs are live-out
  // since we don't want to trust live-in lists.
  if (MBB.succ_empty()) {
    for (MachineInstr *MaybeDead : MaybeDeadCopies) {
      assert(!MRI->isReserved(MaybeDead->getOperand(0).getReg()));
      MaybeDead->eraseFromParent();
      Changed = true;
      ++NumDeletes;
    }
  }

  MaybeDeadCopies.clear();
  AvailCopyMap.clear();
  CopyMap.clear();
  SrcMap.clear();
}

bool MachineCopyPropagation::runOnMachineFunction(MachineFunction &MF) {
  if (skipOptnoneFunction(*MF.getFunction()))
    return false;

  Changed = false;

  TRI = MF.getSubtarget().getRegisterInfo();
  TII = MF.getSubtarget().getInstrInfo();
  MRI = &MF.getRegInfo();

  for (MachineBasicBlock &MBB : MF)
    CopyPropagateBlock(MBB);

  return Changed;
}
