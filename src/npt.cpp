#include "../include/npt.h"

HvState g_hv = {};

static UINT64* PoolAllocPT() {
    LONG idx = InterlockedIncrement(&g_hv.npt_pool_head) - 1;
    if (idx >= NPT_POOL_PAGES) return nullptr;
    RtlZeroMemory(g_hv.npt_pool[idx], PAGE_SIZE);
    return (UINT64*)g_hv.npt_pool[idx];
}

NTSTATUS NptInit(UINT64 max_pa) {
    g_hv.npt_pool_head = 0;
    PHYSICAL_ADDRESS hi = {};
    hi.QuadPart = 0xFFFFFFFFFFFFFFFFLL;

    for (int i = 0; i < NPT_POOL_PAGES; i++) {
        g_hv.npt_pool[i] = MmAllocateContiguousMemory(PAGE_SIZE, hi);
        if (!g_hv.npt_pool[i]) {
            for (int j = 0; j < i; j++) MmFreeContiguousMemory(g_hv.npt_pool[j]);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_hv.npt_pool[i], PAGE_SIZE);
        g_hv.npt_pool_pa[i] = VaToPA(g_hv.npt_pool[i]);
    }

    return NptBuildIdentityMap(max_pa);
}

VOID NptFree() {
    for (int i = 0; i < NPT_POOL_PAGES; i++) {
        if (g_hv.npt_pool[i]) { MmFreeContiguousMemory(g_hv.npt_pool[i]); g_hv.npt_pool[i] = nullptr; }
    }
    if (g_hv.npt_root_va) { MmFreeContiguousMemory(g_hv.npt_root_va); g_hv.npt_root_va = nullptr; }
}

NTSTATUS NptBuildIdentityMap(UINT64 max_pa) {
    PHYSICAL_ADDRESS hi = {};
    hi.QuadPart = 0xFFFFFFFFFFFFFFFFLL;

    UINT64* pml4 = (UINT64*)MmAllocateContiguousMemory(PAGE_SIZE, hi);
    if (!pml4) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(pml4, PAGE_SIZE);
    g_hv.npt_root_va = pml4;
    g_hv.npt_root_pa = VaToPA(pml4);

    UINT64* pdpt_va[PT_ENTRIES] = {};
    UINT64* pd_va[PT_ENTRIES][PT_ENTRIES] = {};

    for (UINT64 pa = 0; pa < max_pa; pa += 0x200000) {
        ULONG pml4i = PML4_INDEX(pa);
        ULONG pdpti = PDPT_INDEX(pa);
        ULONG pdi   = PD_INDEX(pa);

        if (!pdpt_va[pml4i]) {
            UINT64* pdpt = (UINT64*)MmAllocateContiguousMemory(PAGE_SIZE, hi);
            if (!pdpt) { NptFree(); return STATUS_INSUFFICIENT_RESOURCES; }
            RtlZeroMemory(pdpt, PAGE_SIZE);
            pdpt_va[pml4i] = pdpt;
            pml4[pml4i] = VaToPA(pdpt) | NPT_DEFAULT_FLAGS;
        }

        if (!pd_va[pml4i][pdpti]) {
            UINT64* pd = (UINT64*)MmAllocateContiguousMemory(PAGE_SIZE, hi);
            if (!pd) { NptFree(); return STATUS_INSUFFICIENT_RESOURCES; }
            RtlZeroMemory(pd, PAGE_SIZE);
            pd_va[pml4i][pdpti] = pd;
            pdpt_va[pml4i][pdpti] = VaToPA(pd) | NPT_DEFAULT_FLAGS;
        }

        pd_va[pml4i][pdpti][pdi] = pa | NPT_LARGE_2MB | NPT_DEFAULT_FLAGS;
    }

    g_hv.npt_initialized = TRUE;
    return STATUS_SUCCESS;
}

VOID NptHandleNpf(UINT64 gpa, UINT64 error_code) {
    UNREFERENCED_PARAMETER(error_code);
    NptMap4K(gpa & ~0xFFFULL, gpa & ~0xFFFULL, NPT_DEFAULT_FLAGS);
}

BOOLEAN NptMap4K(UINT64 gpa, UINT64 hpa, UINT64 flags) {
    UNREFERENCED_PARAMETER(gpa); UNREFERENCED_PARAMETER(hpa); UNREFERENCED_PARAMETER(flags);
    return FALSE;
}

BOOLEAN NptUnmap4K(UINT64 gpa) {
    UNREFERENCED_PARAMETER(gpa);
    return FALSE;
}
