#pragma once
#include "common.h"

// Check AMD-V is supported and enabled
BOOLEAN SvmIsSupported();

// Enable SVME in EFER on this core
VOID SvmEnable();

// Allocate and initialize PerCoreData for the current processor
NTSTATUS SvmInitCore(UINT32 core_index);

// Free PerCoreData for this core
VOID SvmFreeCore(UINT32 core_index);

// Virtualize the current processor (called via DPC on each core)
// Captures current CPU state into VMCB and executes VMRUN
// Returns to caller with guest executing — OS never notices
VOID SvmVirtualizeCore(KDPC* dpc, PVOID ctx, PVOID arg1, PVOID arg2);

// Devirtualize the current processor (VMCALL with magic value triggers this)
VOID SvmDevirtualizeCore();

// Entry point called from assembly on every #VMEXIT
// regs: pointer to GuestRegs on the host stack
extern "C" VOID SvmExitHandler(GuestRegs* regs, PerCoreData* core);

// VMRUN trampoline — defined in svm_asm.asm
// Saves host regs, loads guest context from VMCB, executes VMRUN
// Returns when #VMEXIT fires; host regs restored by asm stub
extern "C" VOID SvmVmrun(UINT64 vmcb_pa, VOID* host_stack_top);

// VMSAVE/VMLOAD wrappers
extern "C" VOID SvmVmsave(UINT64 vmcb_pa);
extern "C" VOID SvmVmload(UINT64 vmcb_pa);

// VMCALL magic values
#define VMCALL_DEVIRT  0xDEADC0DE
