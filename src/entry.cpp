#include "../include/svm.h"
#include "../include/npt.h"

// Driver cleanup
static VOID HvUnload(PDRIVER_OBJECT driver_object) {
    UNREFERENCED_PARAMETER(driver_object);
    HV_LOG("Unloading hypervisor");

    UINT32 num_cores = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);

    // Devirtualize all cores via IPIs
    // For a real devirt: fire a DPC on each core that does VMMCALL DEVIRT
    // Simplified: just clean up allocations for now
    for (UINT32 i = 0; i < num_cores; i++) {
        SvmFreeCore(i);
    }

    NptFree();

    HV_LOG("Hypervisor unloaded");
}

// DPC context for per-core virtualization
struct VirtDpcCtx {
    volatile LONG cores_done;
    UINT32        total_cores;
};

static KDPC g_dpcs[MAX_PROCESSORS];

static VOID VirtDpcCallback(KDPC* dpc, PVOID ctx, PVOID arg1, PVOID arg2) {
    SvmVirtualizeCore(dpc, ctx, arg1, arg2);
    VirtDpcCtx* vctx = (VirtDpcCtx*)ctx;
    InterlockedIncrement(&vctx->cores_done);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING reg_path) {
    UNREFERENCED_PARAMETER(reg_path);

    HV_LOG("AMD-V Blue-Pill Hypervisor loading");

    driver_object->DriverUnload = HvUnload;

    // Step 1: Check CPU support
    if (!SvmIsSupported()) {
        HV_LOG("AMD SVM not available — aborting");
        return STATUS_NOT_SUPPORTED;
    }

    UINT32 num_cores = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    HV_LOG("Detected %u logical processors", num_cores);
    if (num_cores > MAX_PROCESSORS) {
        HV_LOG("Too many processors (%u > %u)", num_cores, MAX_PROCESSORS);
        return STATUS_NOT_SUPPORTED;
    }
    g_hv.num_cores = num_cores;

    // Step 2: Build NPT identity map
    // Detect installed RAM; map everything up to max physical address
    // Use MmGetPhysicalMemoryRanges to enumerate RAM
    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    UINT64 max_pa = 0;
    if (ranges) {
        for (UINT32 i = 0; ranges[i].BaseAddress.QuadPart || ranges[i].NumberOfBytes.QuadPart; i++) {
            UINT64 end = ranges[i].BaseAddress.QuadPart + ranges[i].NumberOfBytes.QuadPart;
            if (end > max_pa) max_pa = end;
        }
        ExFreePool(ranges);
    }
    if (!max_pa) max_pa = 0x100000000ULL; // fallback: 4GB

    HV_LOG("Building NPT identity map for 0x%llx bytes of physical memory", max_pa);
    NTSTATUS status = NptInit(max_pa);
    if (!NT_SUCCESS(status)) {
        HV_LOG("NptInit failed: 0x%08X", status);
        return status;
    }

    // Step 3: Initialize per-core data for all logical processors
    for (UINT32 i = 0; i < num_cores; i++) {
        status = SvmInitCore(i);
        if (!NT_SUCCESS(status)) {
            HV_LOG("SvmInitCore(%u) failed: 0x%08X", i, status);
            for (UINT32 j = 0; j < i; j++) SvmFreeCore(j);
            NptFree();
            return status;
        }
    }

    // Step 4: Virtualize all cores via DPCs
    // We fire one DPC per processor, pinned to that processor.
    // SvmVirtualizeCore captures state and enters VMRUN loop.
    // After VMRUN completes the blue-pill transition, the DPC returns
    // from the guest's perspective at the same point — the OS never knows.
    VirtDpcCtx vctx = {};
    vctx.total_cores = num_cores;

    for (UINT32 i = 0; i < num_cores; i++) {
        KeInitializeDpc(&g_dpcs[i], VirtDpcCallback, &vctx);
        KeSetTargetProcessorDpc(&g_dpcs[i], (CCHAR)i);
        KeSetImportanceDpc(&g_dpcs[i], HighImportance);
        KeInsertQueueDpc(&g_dpcs[i], nullptr, nullptr);
    }

    // Wait for all cores to complete virtualization
    // (In practice DPCs run immediately at DISPATCH_LEVEL; we spin here)
    // Note: SvmVirtualizeCore is synchronous from the guest's perspective
    // because VMRUN returns to the same instruction — the DPC completes "normally"
    while (InterlockedCompareExchange(&vctx.cores_done, 0, 0) < (LONG)num_cores) {
        YieldProcessor();
        KeMemoryBarrier();
    }

    HV_LOG("All %u cores virtualized — Windows is now a guest", num_cores);
    return STATUS_SUCCESS;
}
