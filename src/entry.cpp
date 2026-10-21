#include "../include/svm.h"
#include "../include/npt.h"

static KDPC g_dpcs[MAX_PROCESSORS];

struct VirtDpcCtx {
    volatile LONG cores_done;
    UINT32        total_cores;
};

static VOID VirtDpcCallback(KDPC* dpc, PVOID ctx, PVOID arg1, PVOID arg2) {
    SvmVirtualizeCore(dpc, ctx, arg1, arg2);
    InterlockedIncrement(&((VirtDpcCtx*)ctx)->cores_done);
}

extern "C" NTSTATUS DriverEntry(PDRIVER_OBJECT driver_object, PUNICODE_STRING reg_path) {
    UNREFERENCED_PARAMETER(driver_object);
    UNREFERENCED_PARAMETER(reg_path);

    if (!SvmIsSupported())
        return STATUS_NOT_SUPPORTED;

    UINT32 num_cores = KeQueryActiveProcessorCountEx(ALL_PROCESSOR_GROUPS);
    if (num_cores > MAX_PROCESSORS)
        return STATUS_NOT_SUPPORTED;
    g_hv.num_cores = num_cores;

    PPHYSICAL_MEMORY_RANGE ranges = MmGetPhysicalMemoryRanges();
    UINT64 max_pa = 0;
    if (ranges) {
        for (UINT32 i = 0; ranges[i].BaseAddress.QuadPart || ranges[i].NumberOfBytes.QuadPart; i++) {
            UINT64 end = ranges[i].BaseAddress.QuadPart + ranges[i].NumberOfBytes.QuadPart;
            if (end > max_pa) max_pa = end;
        }
        ExFreePool(ranges);
    }
    if (!max_pa) max_pa = 0x100000000ULL;

    NTSTATUS status = NptInit(max_pa);
    if (!NT_SUCCESS(status)) return status;

    for (UINT32 i = 0; i < num_cores; i++) {
        status = SvmInitCore(i);
        if (!NT_SUCCESS(status)) {
            for (UINT32 j = 0; j < i; j++) SvmFreeCore(j);
            NptFree();
            return status;
        }
    }

    VirtDpcCtx vctx = {};
    vctx.total_cores = num_cores;

    for (UINT32 i = 0; i < num_cores; i++) {
        KeInitializeDpc(&g_dpcs[i], VirtDpcCallback, &vctx);
        KeSetTargetProcessorDpc(&g_dpcs[i], (CCHAR)i);
        KeSetImportanceDpc(&g_dpcs[i], HighImportance);
        KeInsertQueueDpc(&g_dpcs[i], nullptr, nullptr);
    }

    while (InterlockedCompareExchange(&vctx.cores_done, 0, 0) < (LONG)num_cores) {
        YieldProcessor();
        KeMemoryBarrier();
    }

    return STATUS_SUCCESS;
}
