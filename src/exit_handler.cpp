#include "../include/svm.h"
#include "../include/npt.h"

static UINT64 SafeRdmsr(UINT32 msr) {
    switch (msr) {
    case MSR_EFER:
    case MSR_STAR:
    case MSR_LSTAR:
    case MSR_CSTAR:
    case MSR_SFMASK:
    case MSR_FS_BASE:
    case MSR_GS_BASE:
    case MSR_KERNEL_GS_BASE:
    case MSR_PAT:
    case 0x00000174:
    case 0x00000175:
    case 0x00000176:
    case 0x000001A0:
        return ReadMsr(msr);
    default:
        return 0;
    }
}

static VOID SafeWrmsr(UINT32 msr, UINT64 value) {
    switch (msr) {
    case MSR_EFER:
    case MSR_STAR:
    case MSR_LSTAR:
    case MSR_CSTAR:
    case MSR_SFMASK:
    case MSR_FS_BASE:
    case MSR_GS_BASE:
    case MSR_KERNEL_GS_BASE:
    case MSR_PAT:
    case 0x00000174:
    case 0x00000175:
    case 0x00000176:
    case 0x000001A0:
        WriteMsr(msr, value);
        break;
    default:
        break;
    }
}

static void SkipInstruction(PerCoreData* core) {
    core->vmcb->save.rip = core->vmcb->control.nrip;
}

static void HandleCpuid(GuestRegs* regs, PerCoreData* core) {
    UINT32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    UINT32 leaf    = (UINT32)core->vmcb->save.rax;
    UINT32 subleaf = (UINT32)regs->rcx;

    INT32 raw[4];
    __cpuidex(raw, (INT32)leaf, (INT32)subleaf);
    eax = raw[0]; ebx = raw[1]; ecx = raw[2]; edx = raw[3];

    switch (leaf) {
    case 0x40000000:
        eax = 0x40000001;
        ebx = 0x7263694D;
        ecx = 0x666F736F;
        edx = 0x76482074;
        break;
    case 0x40000001:
        eax = 0x31237648;
        ebx = ecx = edx = 0;
        break;
    case 0x00000001:
        ecx &= ~(1u << 31);
        break;
    case 0x80000001:
        ecx &= ~(1u << 2);
        break;
    default:
        break;
    }

    core->vmcb->save.rax = eax;
    regs->rbx = ebx;
    regs->rcx = ecx;
    regs->rdx = edx;

    SkipInstruction(core);
}

static void HandleMsr(GuestRegs* regs, PerCoreData* core) {
    BOOLEAN is_write = (core->vmcb->control.exit_info1 & 1) != 0;
    UINT32 msr = (UINT32)regs->rcx;

    if (is_write) {
        UINT64 value = ((UINT64)(UINT32)regs->rdx << 32) | (UINT32)core->vmcb->save.rax;
        SafeWrmsr(msr, value);
    } else {
        UINT64 value = SafeRdmsr(msr);
        core->vmcb->save.rax = (UINT32)value;
        regs->rdx = value >> 32;
    }

    SkipInstruction(core);
}

static void HandleHlt(GuestRegs* regs, PerCoreData* core) {
    UNREFERENCED_PARAMETER(regs);
    UINT32 cid = core->core_id;

    if (InterlockedCompareExchange(&g_hv.sipi_pending[cid], 0, 1) == 1) {
        UCHAR vector = (UCHAR)InterlockedExchange8((volatile CHAR*)&g_hv.sipi_vector[cid], 0);
        InterlockedExchange(&g_hv.init_received[cid], 0);

        core->vmcb->save.cs.selector = (UINT16)((UINT32)vector << 8);
        core->vmcb->save.cs.base     = (UINT64)vector << 12;
        core->vmcb->save.cs.limit    = 0xFFFF;
        core->vmcb->save.cs.attrib   = 0x009B;

        core->vmcb->save.rip    = 0;
        core->vmcb->save.cr0    = 0x00000010;
        core->vmcb->save.rflags = 0x00000002;

        UINT16 dsel = (UINT16)((UINT32)vector << 8);
        UINT64 dbase = (UINT64)vector << 12;
        core->vmcb->save.ds.selector = dsel; core->vmcb->save.ds.base = dbase;
        core->vmcb->save.ds.limit    = 0xFFFF; core->vmcb->save.ds.attrib = 0x0093;
        core->vmcb->save.es = core->vmcb->save.ds;
        core->vmcb->save.fs = core->vmcb->save.ds;
        core->vmcb->save.gs = core->vmcb->save.ds;
        core->vmcb->save.ss = core->vmcb->save.ds;
        return;
    }
}

static void HandleVmmcall(PerCoreData* core) {
    UINT64 code = core->vmcb->save.rax;
    if (code == VMCALL_DEVIRT) {
        core->vmcb->save.rax = VMCALL_DEVIRT;
        SkipInstruction(core);
        return;
    }
    SkipInstruction(core);
}

static void HandleNpf(GuestRegs* regs, PerCoreData* core) {
    UNREFERENCED_PARAMETER(regs);
    NptHandleNpf(core->vmcb->control.exit_info2, core->vmcb->control.exit_info1);
}

extern "C" UINT64 SvmExitHandler(GuestRegs* regs, PerCoreData*) {
    UINT32 cid = KeGetCurrentProcessorIndex();
    PerCoreData* core = g_hv.cores[cid];
    UINT64 exit_code = core->vmcb->control.exit_code;

    switch (exit_code) {
    case VMEXIT_CPUID:   HandleCpuid(regs, core);  break;
    case VMEXIT_MSR:     HandleMsr(regs, core);    break;
    case VMEXIT_HLT:     HandleHlt(regs, core);    break;
    case VMEXIT_VMMCALL: HandleVmmcall(core);
        if (core->vmcb->save.rax == VMCALL_DEVIRT) return 1;
        break;
    case VMEXIT_NPF:     HandleNpf(regs, core);    break;
    case VMEXIT_VMRUN:   SkipInstruction(core);    break;
    case VMEXIT_INVALID: return 1;
    default:
        if (core->vmcb->control.nrip) SkipInstruction(core);
        break;
    }

    core->vmcb->control.vmcb_clean = VMCB_CLEAN_ALL;
    if (exit_code == VMEXIT_NPF)
        core->vmcb->control.vmcb_clean &= ~VMCB_CLEAN_NP;

    return 0;
}
