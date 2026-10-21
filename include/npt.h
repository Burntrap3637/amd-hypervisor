#pragma once
#include "common.h"

// AMD NPT uses the same page table structure as standard x86-64
// but with slightly different permission semantics in the PTE

// PTE flags (same bits as x86 paging, NPT adds NX support)
#define NPT_PRESENT    (1ULL << 0)
#define NPT_WRITE      (1ULL << 1)
#define NPT_USER       (1ULL << 2)
#define NPT_LARGE_2MB  (1ULL << 7)   // in PDE: 2MB page
#define NPT_LARGE_1GB  (1ULL << 7)   // in PDPE: 1GB page
#define NPT_NX         (1ULL << 63)

// Standard identity-map PTE: present, writable, user-accessible
#define NPT_DEFAULT_FLAGS (NPT_PRESENT | NPT_WRITE | NPT_USER)

// 512 entries per page table level
#define PT_ENTRIES 512

// Indices into a page table entry for each level
#define PML4_INDEX(gpa) (((gpa) >> 39) & 0x1FF)
#define PDPT_INDEX(gpa) (((gpa) >> 30) & 0x1FF)
#define PD_INDEX(gpa)   (((gpa) >> 21) & 0x1FF)
#define PT_INDEX(gpa)   (((gpa) >> 12) & 0x1FF)

// NPT root = PML4 table (4KB, 512 UINT64 entries)
struct NptPml4 {
    UINT64 entries[PT_ENTRIES];
};

// Pool of pre-allocated page table pages for GIF=0 use
// Allocated at driver init, consumed by NPF handler
NTSTATUS NptInit(UINT64 max_pa);
VOID     NptFree();

// Try to map a 4KB GPA -> HPA in the NPT
// Returns FALSE if pool exhausted (caller must handle gracefully)
BOOLEAN NptMap4K(UINT64 gpa, UINT64 hpa, UINT64 flags);

// Unmap a 4KB GPA (hide from guest — NPF on access)
BOOLEAN NptUnmap4K(UINT64 gpa);

// Build initial identity map: every GPA maps to same HPA, 1:1
// Uses 2MB pages for efficiency (splits on demand in NPF handler)
NTSTATUS NptBuildIdentityMap(UINT64 max_pa);

// Called from NPF exit handler at GIF=0 — must not allocate
VOID NptHandleNpf(UINT64 gpa, UINT64 error_code);
