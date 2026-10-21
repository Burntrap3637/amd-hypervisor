#include "../include/svm.h"
#include "../include/npt.h"

// Safe RDMSR at GIF=0: never fault on unknown MSR — return 0
// A #GP inside the host at GIF=0 is fatal (no NMI watchdog, machine resets)
static UINT64 SafeRdmsr(UINT32 msr) {
    switch (msr) {
    // MSRs we pass through to real hardware
    case MSR_EFER:
    case MSR_STAR:
    case MSR_LSTAR:
    case MSR_SFMASK:
    case MSR_FS_BASE:
    case MSR_GS_BASE:
    case MSR_KERNEL_GS_BASE:
    case MSR_PAT:
    case 0x00000174: // IA32_SYSENTER_CS
    case 0x00000175: // IA32_SYSENTER_ESP
    case 0x00000176: // IA32_SYSENTER_EIP
    case 0x000001A0: // IA32_MISC_ENABLE
    case 0x00000277: // IA32_PAT
    case 0xC0010000: // PERF_CTL0
    case 0xC0010001:
    case 0xC0010004:
    case 0xC0010005:
        return ReadMsr(msr);
    default:
        // Unknown MSR — return 0 instead of faulting
        return 0;
    }
}

// Safe WRMSR at GIF=0: ignore unknown MSRs
static VOID SafeWrmsr(UINT32 msr, UINT64 value) {
    switch (msr) {
    case MSR_EFER:
    case MSR_STAR:
    case MSR_LSTAR:
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
        break; // silently ignore
    }
}

// Advance RIP past the intercepted instruction using NRIP (AMD provides this)
static void SkipInstruction(PerCoreData* core) {
    core->vmcb->save.rip = core->vmcb->control.nrip;
}

// CPUID handler — runs at GIF=0
static void HandleCpuid(GuestRegs* regs, PerCoreData* core) {
    UINT32 eax = 0, ebx = 0, ecx = 0, edx = 0;
    UINT32 leaf    = (UINT32)regs->rcx;  // Note: RAX is in VMCB, rcx here = guest RCX
    UINT32 subleaf = (UINT32)regs->rdx;

    // Guest RAX at exit is in VMCB.save.rax (not in GuestRegs — rax is special on SVM)
    leaf = (UINT32)core->vmcb->save.rax;

    INT32 raw[4];
    __cpuidex(raw, (INT32)leaf, (INT32)subleaf);
    eax = raw[0]; ebx = raw[1]; ecx = raw[2]; edx = raw[3];

    switch (leaf) {
    case 0x40000000:
        // Hypervisor presence leaf — present as Microsoft Hyper-V
        // Windows VBS and Hyper-V detection use this leaf
        eax = 0x40000001;                      // max hypervisor leaf
        ebx = 0x7263694D; // "Micr"
        ecx = 0x666F736F; // "osof"
        edx = 0x76482074; // "t Hv"
        break;

    case 0x40000001:
        // Hyper-V interface signature: "Hv#1"
        eax = 0x31237648;
        ebx = ecx = edx = 0;
        break;

    case 0x00000001:
        // Patch: hide HYPERVISOR bit (ECX bit 31) and hide AMD-V availability (ECX bit 5)
        // Also clear SVM feature in ECX if Windows would use it to detect us
        ecx &= ~(1u << 31); // clear HYPERVISOR bit
        // Keep the rest intact — Windows needs APIC, SSE, etc.
        break;

    case 0x80000001:
        // AMD feature flags — hide SVM (bit 2 of ECX) from guest
        ecx &= ~(1u << 2); // clear SVM bit so guest thinks AMD-V is off
        break;

    default:
        break; // pass through unmodified
    }

    // Write results back — on SVM, CPUID output goes to rax/rbx/rcx/rdx
    // RAX goes to VMCB.save.rax; rbx/rcx/rdx go to GuestRegs
    core->vmcb->save.rax = eax;
    regs->rbx = ebx;
    regs->rcx = ecx;
    regs->rdx = edx;

    SkipInstruction(core);
}

// MSR handler — runs at GIF=0
// exit_info1: bit 0 = 0 for read, 1 for write
// exit_info2 (AMD): ECX (MSR number) is in the VMCB exit context
static void HandleMsr(GuestRegs* regs, PerCoreData* core) {
    BOOLEAN is_write = (core->vmcb->control.exit_info1 & 1) != 0;
    UINT32 msr = (UINT32)regs->rcx;

    if (is_write) {
        // Guest writing MSR: value is EDX:EAX
        UINT64 value = ((UINT64)(UINT32)regs->rdx << 32) | (UINT32)core->vmcb->save.rax;
        SafeWrmsr(msr, value);
    } else {
        // Guest reading MSR: return EDX:EAX
        UINT64 value = SafeRdmsr(msr);
        core->vmcb->save.rax = (UINT32)value;
        regs->rdx = value >> 32;
    }

    SkipInstruction(core);
}

// HLT handler — check for pending SIPI from BSP
static void HandleHlt(GuestRegs* regs, PerCoreData* core) {
    UNREFERENCED_PARAMETER(regs);
    UINT32 cid = core->core_id;

    if (InterlockedCompareExchange(&g_hv.sipi_pending[cid], 0, 1) == 1) {
        // SIPI was queued by another core's IPI intercept
        UCHAR vector = (UCHAR)InterlockedExchange8(
            (volatile CHAR*)&g_hv.sipi_vector[cid], 0);
        InterlockedExchange(&g_hv.init_received[cid], 0);

        // Reset guest to real mode at vector << 12, RIP = 0
        // CS.selector = vector << 8, CS.base = vector << 12
        core->vmcb->save.cs.selector = (UINT16)((UINT32)vector << 8);
        core->vmcb->save.cs.base     = (UINT64)vector << 12;
        core->vmcb->save.cs.limit    = 0xFFFF;
        core->vmcb->save.cs.attrib   = 0x009B; // 16-bit code, present, RW

        core->vmcb->save.rip    = 0;
        core->vmcb->save.cr0    = 0x00000010; // real mode (ET bit)
        core->vmcb->save.rflags = 0x00000002; // reserved bit set

        // Real-mode data segments
        UINT16 ds_sel = (UINT16)((UINT32)vector << 8);
        UINT64 ds_base = (UINT64)vector << 12;
        core->vmcb->save.ds.selector = ds_sel; core->vmcb->save.ds.base = ds_base;
        core->vmcb->save.ds.limit    = 0xFFFF; core->vmcb->save.ds.attrib = 0x0093;
        core->vmcb->save.es = core->vmcb->save.ds;
        core->vmcb->save.fs = core->vmcb->save.ds;
        core->vmcb->save.gs = core->vmcb->save.ds;
        core->vmcb->save.ss = core->vmcb->save.ds;

        // Fall through to VMRUN without advancing RIP — guest executes real-mode entry
        return;
    }

    // No SIPI — don't advance RIP, re-execute HLT on next VMRUN
    // (the guest will exit again immediately, and we check again — tight but harmless)
}

// VMMCALL handler
static void HandleVmmcall(GuestRegs* regs, PerCoreData* core) {
    UINT64 call_code = core->vmcb->save.rax;

    if (call_code == VMCALL_DEVIRT) {
        // Devirtualize: signal SvmVmrun to exit its loop
        // Return a sentinel in RAX so the asm loop breaks
        core->vmcb->save.rax = VMCALL_DEVIRT;
        SkipInstruction(core);
        // The asm stub checks the return value of SvmExitHandler:
        // we return 1 to signal stop
        return;
    }

    SkipInstruction(core);
}

// NPF handler — nested page fault; runs at GIF=0
// exit_info1 = NPF error code (like a page fault error code)
// exit_info2 = faulting guest physical address
static void HandleNpf(GuestRegs* regs, PerCoreData* core) {
    UNREFERENCED_PARAMETER(regs);
    UINT64 gpa        = core->vmcb->control.exit_info2;
    UINT64 error_code = core->vmcb->control.exit_info1;
    NptHandleNpf(gpa, error_code);
    // Don't skip instruction — fault should be resolved and guest re-executes
}

// Main exit handler — called from SvmVmrun asm stub on every #VMEXIT
// Returns 0 to continue VMX loop, non-zero to devirtualize
extern "C" UINT64 SvmExitHandler(GuestRegs* regs, PerCoreData* /*unused*/) {
    UINT32 cid = KeGetCurrentProcessorIndex();
    PerCoreData* core = g_hv.cores[cid];

    UINT64 exit_code = core->vmcb->control.exit_code;

    switch (exit_code) {
    case VMEXIT_CPUID:
        HandleCpuid(regs, core);
        break;

    case VMEXIT_MSR:
        HandleMsr(regs, core);
        break;

    case VMEXIT_HLT:
        HandleHlt(regs, core);
        break;

    case VMEXIT_VMMCALL:
        HandleVmmcall(regs, core);
        if (core->vmcb->save.rax == VMCALL_DEVIRT)
            return 1; // signal asm loop to exit
        break;

    case VMEXIT_NPF:
        HandleNpf(regs, core);
        break;

    case VMEXIT_VMRUN:
        // Guest tried to execute VMRUN — we intercept it to prevent
        // nested virtualization attempts
        SkipInstruction(core);
        break;

    case VMEXIT_INVALID:
        // Bad VMCB — fatal; return 1 to bail out
        return 1;

    default:
        // Unknown/unhandled exit — skip the instruction and hope for the best
        // Better: inject a #UD into the guest
        if (core->vmcb->control.nrip)
            SkipInstruction(core);
        break;
    }

    // Set VMCB clean bits to avoid unnecessary state reload on next VMRUN
    // We only dirty the registers we touched; mark everything else clean
    core->vmcb->control.vmcb_clean = VMCB_CLEAN_ALL;
    // Clear clean bits for fields we modified
    if (exit_code == VMEXIT_NPF)
        core->vmcb->control.vmcb_clean &= ~VMCB_CLEAN_NP;

    return 0; // continue
}
