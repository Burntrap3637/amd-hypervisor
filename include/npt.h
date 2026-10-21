#pragma once
#include "common.h"

#define NPT_PRESENT    (1ULL << 0)
#define NPT_WRITE      (1ULL << 1)
#define NPT_USER       (1ULL << 2)
#define NPT_LARGE_2MB  (1ULL << 7)
#define NPT_NX         (1ULL << 63)
#define NPT_DEFAULT_FLAGS (NPT_PRESENT | NPT_WRITE | NPT_USER)

#define PT_ENTRIES 512

#define PML4_INDEX(gpa) (((gpa) >> 39) & 0x1FF)
#define PDPT_INDEX(gpa) (((gpa) >> 30) & 0x1FF)
#define PD_INDEX(gpa)   (((gpa) >> 21) & 0x1FF)
#define PT_INDEX(gpa)   (((gpa) >> 12) & 0x1FF)

NTSTATUS NptInit(UINT64 max_pa);
VOID     NptFree();
BOOLEAN  NptMap4K(UINT64 gpa, UINT64 hpa, UINT64 flags);
BOOLEAN  NptUnmap4K(UINT64 gpa);
NTSTATUS NptBuildIdentityMap(UINT64 max_pa);
VOID     NptHandleNpf(UINT64 gpa, UINT64 error_code);
