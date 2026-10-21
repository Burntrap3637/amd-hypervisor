#pragma once
#include "common.h"

BOOLEAN SvmIsSupported();
VOID    SvmEnable();
NTSTATUS SvmInitCore(UINT32 core_index);
VOID    SvmFreeCore(UINT32 core_index);
VOID    SvmVirtualizeCore(KDPC* dpc, PVOID ctx, PVOID arg1, PVOID arg2);

extern "C" UINT64 SvmExitHandler(GuestRegs* regs, PerCoreData* core);
extern "C" VOID   SvmVmrun(UINT64 vmcb_pa, VOID* host_stack_top);
extern "C" VOID   SvmVmsave(UINT64 vmcb_pa);
extern "C" VOID   SvmVmload(UINT64 vmcb_pa);

#define VMCALL_DEVIRT 0xDEADC0DE
