#include "../include/svm.h"
#include "../include/npt.h"

BOOLEAN SvmIsSupported() {
    UINT32 eax, ebx, ecx, edx;
    Cpuid(0x80000001, &eax, &ebx, &ecx, &edx);
    if (!(ecx & (1u << 2))) return FALSE;
    if (ReadMsr(MSR_VM_CR) & (1ULL << 4)) return FALSE;
    return TRUE;
}

VOID SvmEnable() {
    WriteMsr(MSR_EFER, ReadMsr(MSR_EFER) | EFER_SVME);
}

static VOID SetupMsrpm(UINT8* msrpm) {
    RtlZeroMemory(msrpm, MSRPM_SIZE);

    auto Intercept = [&](UINT32 msr, BOOLEAN rd, BOOLEAN wr) {
        UINT32 offset, local;
        if      (msr <= 0x1FFF)                             { offset = 0x000; local = msr; }
        else if (msr >= 0xC0000000 && msr <= 0xC0001FFF)   { offset = 0x800; local = msr - 0xC0000000; }
        else if (msr >= 0xC0010000 && msr <= 0xC0011FFF)   { offset = 0x1000; local = msr - 0xC0010000; }
        else return;
        UINT32 bit = local * 2;
        UINT8& byte = msrpm[offset + bit / 8];
        UINT8 shift = (UINT8)(bit % 8);
        if (rd) byte |= (UINT8)(1u << shift);
        if (wr) byte |= (UINT8)(1u << (shift + 1));
    };

    Intercept(MSR_EFER, TRUE, TRUE);
}

static VOID SetupIopm(UINT8* iopm) {
    RtlZeroMemory(iopm, IOPM_SIZE);
}

NTSTATUS SvmInitCore(UINT32 core_index) {
    PHYSICAL_ADDRESS hi = {};
    hi.QuadPart = 0xFFFFFFFFFFFFFFFFLL;

    PerCoreData* core = (PerCoreData*)ExAllocatePool2(POOL_FLAG_NON_PAGED, sizeof(PerCoreData), HV_POOL_TAG);
    if (!core) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(core, sizeof(PerCoreData));
    core->core_id = core_index;

    core->vmcb = (Vmcb*)MmAllocateContiguousMemory(sizeof(Vmcb), hi);
    if (!core->vmcb) { ExFreePool(core); return STATUS_INSUFFICIENT_RESOURCES; }
    RtlZeroMemory(core->vmcb, sizeof(Vmcb));
    core->vmcb_pa = VaToPA(core->vmcb);

    core->host_vmcb = (Vmcb*)MmAllocateContiguousMemory(sizeof(Vmcb), hi);
    if (!core->host_vmcb) goto cleanup;
    RtlZeroMemory(core->host_vmcb, sizeof(Vmcb));
    core->host_vmcb_pa = VaToPA(core->host_vmcb);

    core->msrpm = (UINT8*)MmAllocateContiguousMemory(MSRPM_SIZE, hi);
    if (!core->msrpm) goto cleanup;
    core->msrpm_pa = VaToPA(core->msrpm);
    SetupMsrpm(core->msrpm);

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

VOID SvmVirtualizeCore(KDPC* dpc, PVOID ctx, PVOID arg1, PVOID arg2) {
    UNREFERENCED_PARAMETER(dpc); UNREFERENCED_PARAMETER(ctx);
    UNREFERENCED_PARAMETER(arg1); UNREFERENCED_PARAMETER(arg2);

    UINT32 cid = KeGetCurrentProcessorIndex();
    PerCoreData* core = g_hv.cores[cid];
    if (!core) return;

    SvmEnable();
    WriteMsr(MSR_VM_HSAVE_PA, core->host_vmcb_pa);

    SvmVmsave(core->vmcb_pa);

    VmcbSave& s = core->vmcb->save;
    s.cr0   = __readcr0();
    s.cr3   = __readcr3();
    s.cr4   = __readcr4();
    s.dr7   = __readdr(7);
    s.dr6   = __readdr(6);
    s.rflags = __readeflags();
    s.efer  = ReadMsr(MSR_EFER) | EFER_SVME;
    s.g_pat = ReadMsr(MSR_PAT);
    s.cpl   = 0;

    VmcbControl& c = core->vmcb->control;
    c.intercept_misc1 = INTERCEPT_CPUID | INTERCEPT_HLT | INTERCEPT_MSR;
    c.intercept_misc2 = INTERCEPT_VMRUN | INTERCEPT_VMMCALL;
    c.msrpm_base_pa   = core->msrpm_pa;
    c.iopm_base_pa    = core->iopm_pa;
    c.np_enable       = NP_ENABLE;
    c.n_cr3           = g_hv.npt_root_pa;
    c.guest_asid      = 1;
    c.tsc_offset      = (UINT64)(-(INT64)150);
    c.tlb_control     = TLB_CONTROL_FLUSH_ALL;
    c.vmcb_clean      = 0;

    core->virtualized = TRUE;

    VOID* host_stack_top = (UINT8*)core->host_stack + sizeof(core->host_stack) - 8;
    SvmVmrun(core->vmcb_pa, host_stack_top);

    WriteMsr(MSR_EFER, ReadMsr(MSR_EFER) & ~EFER_SVME);
    core->virtualized = FALSE;
}
