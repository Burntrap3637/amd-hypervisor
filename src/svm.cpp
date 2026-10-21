#include "../include/svm.h"
#include "../include/npt.h"

// Check CPU supports AMD-V
BOOLEAN SvmIsSupported() {
    UINT32 eax, ebx, ecx, edx;
    Cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 2))) {
        HV_LOG("SVM not supported (CPUID 0x80000001 ECX bit 2 = 0)");
        return FALSE;
    }
    // Check VM_CR MSR — SVMDIS bit (bit 4) must be 0
    UINT64 vm_cr = ReadMsr(MSR_VM_CR);
    if (vm_cr & (1ULL << 4)) {
        HV_LOG("SVM disabled in BIOS (VM_CR.SVMDIS set)");
        return FALSE;
    }
    return TRUE;
}

VOID SvmEnable() {
    // Set EFER.SVME (bit 12) to enable SVM on this core
    UINT64 efer = ReadMsr(MSR_EFER);
    efer |= EFER_SVME;
    WriteMsr(MSR_EFER, efer);
}

// Build the MSRPM (MSR Permission Map)
// 8KB bitmap, 2 bits per MSR (bit 0 = intercept read, bit 1 = intercept write)
// We intercept only MSRs we care about; pass everything else through
static VOID SetupMsrpm(UINT8* msrpm) {
    RtlZeroMemory(msrpm, MSRPM_SIZE);

    // Helper: set intercept bits for an MSR
    // Range 0: MSR 0x00000000–0x00001FFF → bytes 0x000–0x7FF
    // Range 1: MSR 0xC0000000–0xC0001FFF → bytes 0x800–0xFFF
    // Range 2: MSR 0xC0010000–0xC0011FFF → bytes 0x1000–0x17FF
    auto InterceptMsr = [&](UINT32 msr, BOOLEAN intercept_read, BOOLEAN intercept_write) {
        UINT32 offset;
        UINT32 local_idx;
        if (msr <= 0x1FFF) {
            offset = 0; local_idx = msr;
        } else if (msr >= 0xC0000000 && msr <= 0xC0001FFF) {
            offset = 0x800; local_idx = msr - 0xC0000000;
        } else if (msr >= 0xC0010000 && msr <= 0xC0011FFF) {
            offset = 0x1000; local_idx = msr - 0xC0010000;
        } else {
            return; // MSR not covered by MSRPM
        }
        UINT32 bit = local_idx * 2;
        UINT32 byte_off = offset + bit / 8;
        UINT8 bit_in_byte = (UINT8)(bit % 8);
        if (intercept_read)  msrpm[byte_off] |= (UINT8)(1u << bit_in_byte);
        if (intercept_write) msrpm[byte_off] |= (UINT8)(1u << (bit_in_byte + 1));
    };

    // Intercept EFER reads/writes so we can hide SVME from the guest
    InterceptMsr(MSR_EFER, TRUE, TRUE);

    // Intercept LSTAR so we can log syscall entry points if desired
    // (disabled by default — enable for debugging)
    // InterceptMsr(MSR_LSTAR, TRUE, TRUE);
}

// Build IOPM — all zeros = no I/O interception
static VOID SetupIopm(UINT8* iopm) {
    RtlZeroMemory(iopm, IOPM_SIZE);
}

// Capture current CPU state into a VMCB save area
// This is the blue-pill moment: we snapshot where we are so the guest
// (the running Windows kernel) picks up executing at exactly the same RIP
static VOID CaptureCpuState(Vmcb* vmcb) {
    VmcbSave& s = vmcb->save;

    // Capture segment registers via inline asm helpers
    // We read the hidden descriptor state using VMSAVE trick:
    // First VMSAVE to a temp buffer, then copy into our VMCB
    // Actually, since we configure the guest VMCB ourselves, we populate manually:

    // CR registers
    s.cr0   = __readcr0();
    s.cr3   = __readcr3();
    s.cr4   = __readcr4();
    s.cr2   = 0; // not critical

    // DR registers
    s.dr7   = __readdr(7);
    s.dr6   = __readdr(6);

    // RFLAGS and RIP — RIP will be patched by the asm stub after VMRUN
    // We set RIP to the instruction after our VMRUN call
    // (the asm stub handles this by executing VMRUN; the guest's RIP is the
    //  return address visible to the caller of SvmVmrun)
    s.rflags = __readeflags();

    // RAX — saved separately in VMCB (not in GuestRegs)
    s.rax = 0; // set by asm stub from actual RAX

    // Stack pointer — asm stub patches this from actual RSP
    s.rsp = 0;

    // Segments — read selector registers
    s.cs.selector = (UINT16)__segmentlimit(0x08); // placeholder
    s.ss.selector = 0x10;
    s.ds.selector = 0x2B;
    s.es.selector = 0x2B;

    // EFER
    s.efer = ReadMsr(MSR_EFER) | EFER_SVME;

    // MSRs
    s.star         = ReadMsr(MSR_STAR);
    s.lstar        = ReadMsr(MSR_LSTAR);
    s.cstar        = ReadMsr(MSR_CSTAR);
    s.sfmask       = ReadMsr(MSR_SFMASK);
    s.kernel_gs_base = ReadMsr(MSR_KERNEL_GS_BASE);
    s.sysenter_cs  = ReadMsr(0x174);
    s.sysenter_esp = ReadMsr(0x175);
    s.sysenter_eip = ReadMsr(0x176);
    s.g_pat        = ReadMsr(MSR_PAT);

    // CPL: we're in ring 0
    s.cpl = 0;
}

// Use VMSAVE to capture the full hidden segment state from the hardware
// This gets the real CS/SS/DS/ES/FS/GS/LDTR/GDTR/IDTR/TR base+limit+attrib
static VOID CaptureHiddenSegmentState(Vmcb* vmcb) {
    // VMSAVE to a temporary VMCB, then copy the segments we care about
    // We pass our actual VMCB PA — VMSAVE will fill in the save area
    // (We do this before setting up our control area to avoid overwriting it)
    SvmVmsave(VaToPA(vmcb));
    // After VMSAVE, vmcb->save now contains the true current CPU state
    // including all hidden descriptor fields. Our CaptureCpuState() already
    // set CR0/CR3/CR4/EFER/MSRs. The VMSAVE will overwrite segments/IDTR/GDTR
    // with the correct hardware state — which is exactly what we want.
}

NTSTATUS SvmInitCore(UINT32 core_index) {
    PHYSICAL_ADDRESS hi = {};
    hi.QuadPart = 0xFFFFFFFFFFFFFFFFLL;

    PerCoreData* core = (PerCoreData*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(PerCoreData), HV_POOL_TAG);
    if (!core) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(core, sizeof(PerCoreData));
    core->core_id = core_index;

    // Allocate guest VMCB (4KB aligned, physically contiguous)
    core->vmcb = (Vmcb*)MmAllocateContiguousMemory(sizeof(Vmcb), hi);
    if (!core->vmcb) { ExFreePool(core); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(core->vmcb, sizeof(Vmcb));
    core->vmcb_pa = VaToPA(core->vmcb);

    // Host save area: MSR_VM_HSAVE_PA — hardware saves host state here on VMRUN
    core->host_vmcb = (Vmcb*)MmAllocateContiguousMemory(sizeof(Vmcb), hi);
    if (!core->host_vmcb) {
        MmFreeContiguousMemory(core->vmcb);
        ExFreePool(core);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    RtlZeroMemory(core->host_vmcb, sizeof(Vmcb));
    core->host_vmcb_pa = VaToPA(core->host_vmcb);

    // MSRPM
    core->msrpm = (UINT8*)MmAllocateContiguousMemory(MSRPM_SIZE, hi);
    if (!core->msrpm) goto cleanup;
    core->msrpm_pa = VaToPA(core->msrpm);
    SetupMsrpm(core->msrpm);

    // IOPM
    core->iopm = (UINT8*)MmAllocateContiguousMemory(IOPM_SIZE, hi);
    if (!core->iopm) goto cleanup;
    core->iopm_pa = VaToPA(core->iopm);
    SetupIopm(core->iopm);

    g_hv.cores[core_index] = core;
    return STATUS_SUCCESS;

cleanup:
    SvmFreeCore(core_index);
    return STATUS_INSUFFICIENT_RESOURCES;
}

VOID SvmFreeCore(UINT32 core_index) {
    PerCoreData* core = g_hv.cores[core_index];
    if (!core) return;
    if (core->vmcb)      MmFreeContiguousMemory(core->vmcb);
    if (core->host_vmcb) MmFreeContiguousMemory(core->host_vmcb);
    if (core->msrpm)     MmFreeContiguousMemory(core->msrpm);
    if (core->iopm)      MmFreeContiguousMemory(core->iopm);
    ExFreePool(core);
    g_hv.cores[core_index] = nullptr;
}

// DPC callback: runs on each logical processor to perform virtualization
// This is called from DriverEntry via KeInsertQueueDpc for each core
VOID SvmVirtualizeCore(KDPC* dpc, PVOID ctx, PVOID arg1, PVOID arg2) {
    UNREFERENCED_PARAMETER(dpc); UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(arg1); UNREFERENCED_PARAMETER(arg2);

    UINT32 cid = KeGetCurrentProcessorIndex();
    PerCoreData* core = g_hv.cores[cid];
    if (!core) return;

    // Enable SVM on this core
    SvmEnable();

    // Set host save area physical address
    WriteMsr(MSR_VM_HSAVE_PA, core->host_vmcb_pa);

    // Capture current CPU state into VMCB using VMSAVE
    // VMSAVE fills in: CS/SS/DS/ES/FS/GS/TR/LDTR/GDTR/IDTR
    //                  KERNEL_GS_BASE, STAR, LSTAR, CSTAR, SFMASK, SYSENTER_*
    CaptureHiddenSegmentState(core->vmcb);

    // Fill in the rest manually (CRs, EFER, MSRs not covered by VMSAVE)
    CaptureCpuState(core->vmcb);

    // Control area setup
    VmcbControl& c = core->vmcb->control;

    // Intercepts
    c.intercept_misc1 = INTERCEPT_CPUID |
                        INTERCEPT_HLT   |
                        INTERCEPT_MSR;

    c.intercept_misc2 = INTERCEPT_VMRUN |
                        INTERCEPT_VMMCALL;

    c.intercept_exceptions = 0; // no exception intercepts for now

    // MSRPM and IOPM
    c.msrpm_base_pa = core->msrpm_pa;
    c.iopm_base_pa  = core->iopm_pa;

    // NPT
    c.np_enable = NP_ENABLE;
    c.n_cr3     = g_hv.npt_root_pa;

    // ASID: must be non-zero (0 = host)
    c.guest_asid = 1;

    // TSC offset: subtract avg exit cost to compensate timing leakage
    // ~150 cycles on Ryzen 7; measured empirically
    c.tsc_offset = (UINT64)(-(INT64)150);

    // TLB: flush all on first VMRUN
    c.tlb_control = TLB_CONTROL_FLUSH_ALL;

    // VMCB clean bits: none valid yet (first run)
    c.vmcb_clean = 0;

    core->virtualized = TRUE;

    // Enter the VMRUN loop — this function does not return until devirtualization
    // The asm stub captures host RSP/RIP, executes VMRUN, handles exits in a loop
    VOID* host_stack_top = (UINT8*)core->host_stack + sizeof(core->host_stack) - 8;
    SvmVmrun(core->vmcb_pa, host_stack_top);

    // If we get here, the guest executed VMCALL with VMCALL_DEVIRT
    // Restore EFER (clear SVME) and return
    UINT64 efer = ReadMsr(MSR_EFER);
    efer &= ~EFER_SVME;
    WriteMsr(MSR_EFER, efer);

    core->virtualized = FALSE;
}

VOID SvmDevirtualizeCore() {
    // Issue VMCALL with devirt magic — the exit handler will signal the asm loop to stop
    __vmx_vmcall(VMCALL_DEVIRT, 0, 0, 0); // not VMX, but same pattern: inline asm
    // Actual VMMCALL instruction generated in entry.cpp or via intrinsic
}
