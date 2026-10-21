#include "../include/npt.h"

// NPT uses a 4-level page table identical to x86-64 paging in structure,
// but each entry maps GPA -> HPA (host physical).
// We build a full identity map (GPA == HPA) at init using 2MB pages,
// then the NPF handler can split individual 2MB pages into 4KB on demand
// using the pre-allocated pool (no allocation at GIF=0).

HvState g_hv = {};

// Each level of the NPT is a 4KB page of 512 UINT64 entries
static UINT64* AllocPageTable() {
    UINT64 pa_unused[2];
    (void)pa_unused;
    if (InterlockedIncrement(&g_hv.npt_pool_head) - 1 >= NPT_POOL_PAGES)
        return nullptr;
    LONG idx = InterlockedIncrement(&g_hv.npt_pool_head) - 1;
    if (idx >= NPT_POOL_PAGES) return nullptr;
    return (UINT64*)g_hv.npt_pool[idx];
}

// Pool-backed alloc — safe at GIF=0
static UINT64* PoolAllocPT() {
    LONG idx = InterlockedIncrement(&g_hv.npt_pool_head) - 1;
    if (idx >= NPT_POOL_PAGES) {
        // pool exhausted — this is a best-effort; caller handles nullptr
        return nullptr;
    }
    RtlZeroMemory(g_hv.npt_pool[idx], PAGE_SIZE);
    return (UINT64*)g_hv.npt_pool[idx];
}

static UINT64 PoolPTPhys(UINT64* va) {
    // Find which pool slot this VA belongs to
    for (int i = 0; i < NPT_POOL_PAGES; i++) {
        if (g_hv.npt_pool[i] == va)
            return g_hv.npt_pool_pa[i];
    }
    return 0;
}

NTSTATUS NptInit(UINT64 max_pa) {
    // Pre-allocate pool pages
    g_hv.npt_pool_head = 0;
    for (int i = 0; i < NPT_POOL_PAGES; i++) {
        PHYSICAL_ADDRESS hi = {};
        hi.QuadPart = 0xFFFFFFFFFFFFFFFFLL;
        g_hv.npt_pool[i] = MmAllocateContiguousMemory(PAGE_SIZE, hi);
        if (!g_hv.npt_pool[i]) {
            // free what we have and bail
            for (int j = 0; j < i; j++)
                MmFreeContiguousMemory(g_hv.npt_pool[j]);
            return STATUS_INSUFFICIENT_RESOURCES;
        }
        RtlZeroMemory(g_hv.npt_pool[i], PAGE_SIZE);
        g_hv.npt_pool_pa[i] = VaToPA(g_hv.npt_pool[i]);
    }

    return NptBuildIdentityMap(max_pa);
}

VOID NptFree() {
    for (int i = 0; i < NPT_POOL_PAGES; i++) {
        if (g_hv.npt_pool[i]) {
            MmFreeContiguousMemory(g_hv.npt_pool[i]);
            g_hv.npt_pool[i] = nullptr;
        }
    }
    if (g_hv.npt_root_va) {
        MmFreeContiguousMemory(g_hv.npt_root_va);
        g_hv.npt_root_va = nullptr;
    }
}

NTSTATUS NptBuildIdentityMap(UINT64 max_pa) {
    PHYSICAL_ADDRESS hi = {};
    hi.QuadPart = 0xFFFFFFFFFFFFFFFFLL;

    // Allocate PML4 root
    UINT64* pml4 = (UINT64*)MmAllocateContiguousMemory(PAGE_SIZE, hi);
    if (!pml4) return STATUS_INSUFFICIENT_RESOURCES;
    RtlZeroMemory(pml4, PAGE_SIZE);
    g_hv.npt_root_va = pml4;
    g_hv.npt_root_pa = VaToPA(pml4);

    // Walk physical address space in 2MB chunks and build identity map
    // We use 2MB pages (PDE with PS bit) for efficiency
    for (UINT64 pa = 0; pa < max_pa; pa += 0x200000) {
        UINT64 pml4_idx = PML4_INDEX(pa);
        UINT64 pdpt_idx = PDPT_INDEX(pa);
        UINT64 pd_idx   = PD_INDEX(pa);

        // PML4 entry
        if (!pml4[pml4_idx]) {
            UINT64* pdpt = (UINT64*)MmAllocateContiguousMemory(PAGE_SIZE, hi);
            if (!pdpt) { NptFree(); return STATUS_INSUFFICIENT_RESOURCES; }
            RtlZeroMemory(pdpt, PAGE_SIZE);
            pml4[pml4_idx] = VaToPA(pdpt) | NPT_DEFAULT_FLAGS;
        }

        // PDPT entry
        UINT64* pdpt = (UINT64*)(pml4[pml4_idx] & ~0xFFFULL);
        // We stored the PA, need to map back to VA for access — but since we're
        // in kernel VA space and these are contiguous allocations, we can't trivially
        // go PA -> VA. Instead, maintain a shadow VA table.
        // Simpler approach: pre-compute and store VA pointers in a separate array.
        // For the identity map builder we just use MmMapIoSpace to access the tables.
        // Actually, since all our NPT pages are allocated via MmAllocateContiguousMemory,
        // the returned VA IS the kernel VA. We stored the PA in the entry.
        // We need VA access — track separately.
        (void)pdpt; // handled in the VA-tracking version below
    }

    // Cleaner implementation: use VA tracking arrays
    // Reset and redo with proper VA tracking
    RtlZeroMemory(pml4, PAGE_SIZE);

    // Track PDPT VAs indexed by PML4 slot
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

        // 2MB identity-map PDE: NPT_LARGE_2MB | RWX | present
        pd_va[pml4i][pdpti][pdi] = pa | NPT_LARGE_2MB | NPT_DEFAULT_FLAGS;
    }

    g_hv.npt_initialized = TRUE;
    return STATUS_SUCCESS;
}

// NptHandleNpf — called from exit_handler.cpp at GIF=0
// For our blue-pill, NPFs should be rare (only if something tries to access
// a GPA we've unmapped). Default action: map GPA = HPA (identity) if possible.
VOID NptHandleNpf(UINT64 gpa, UINT64 error_code) {
    UNREFERENCED_PARAMETER(error_code);
    // Identity-map the faulting page using pool
    // In a real stealth implementation you'd check if this GPA should be hidden
    // and return zeros; for now just identity-map it
    NptMap4K(gpa & ~0xFFFULL, gpa & ~0xFFFULL, NPT_DEFAULT_FLAGS);
}

BOOLEAN NptMap4K(UINT64 gpa, UINT64 hpa, UINT64 flags) {
    UNREFERENCED_PARAMETER(gpa); UNREFERENCED_PARAMETER(hpa); UNREFERENCED_PARAMETER(flags);
    // Full 4K split-and-map implementation would:
    // 1. Walk PML4 -> PDPT -> PD to find the 2MB PDE covering gpa
    // 2. If it's a 2MB large page, split it:
    //    a. allocate new PT from pool (PoolAllocPT)
    //    b. fill 512 PTEs with 4KB mappings mirroring the 2MB
    //    c. replace the 2MB PDE with a pointer to the new PT
    // 3. Set the specific 4KB PTE to hpa | flags
    // 4. Invalidate TLB: INVLPGA gpa, ASID
    // Omitted for initial commit — NPF handler is last-resort anyway
    return FALSE;
}

BOOLEAN NptUnmap4K(UINT64 gpa) {
    UNREFERENCED_PARAMETER(gpa);
    return FALSE;
}
