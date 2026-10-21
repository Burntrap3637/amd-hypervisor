#pragma once
#include <ntddk.h>
#include <intrin.h>
#include "vmcb.h"

#define HV_POOL_TAG    'pVMA'
#define MAX_PROCESSORS 64
#define MSRPM_SIZE     8192
#define IOPM_SIZE      12288
#define NPT_POOL_PAGES 256

#pragma pack(push, 1)
struct GuestRegs {
    UINT64 r15;
    UINT64 r14;
    UINT64 r13;
    UINT64 r12;
    UINT64 r11;
    UINT64 r10;
    UINT64 r9;
    UINT64 r8;
    UINT64 rdi;
    UINT64 rsi;
    UINT64 rbp;
    UINT64 rbx;
    UINT64 rdx;
    UINT64 rcx;
};
static_assert(sizeof(GuestRegs) == 14 * 8);
#pragma pack(pop)

struct PerCoreData {
    Vmcb*    vmcb;
    Vmcb*    host_vmcb;
    UINT64   vmcb_pa;
    UINT64   host_vmcb_pa;
    UINT64   host_stack[512];
    UINT8*   msrpm;
    UINT8*   iopm;
    UINT64   msrpm_pa;
    UINT64   iopm_pa;
    BOOLEAN  virtualized;
    UINT32   core_id;
};

struct HvState {
    PerCoreData* cores[MAX_PROCESSORS];
    UINT32       num_cores;
    UINT64       npt_root_pa;
    VOID*        npt_root_va;
    BOOLEAN      npt_initialized;
    VOID*        npt_pool[NPT_POOL_PAGES];
    UINT64       npt_pool_pa[NPT_POOL_PAGES];
    volatile LONG npt_pool_head;
    volatile LONG sipi_pending[MAX_PROCESSORS];
    volatile UCHAR sipi_vector[MAX_PROCESSORS];
    volatile LONG init_received[MAX_PROCESSORS];
};

extern HvState g_hv;

static inline UINT64 VaToPA(VOID* va) {
    return MmGetPhysicalAddress(va).QuadPart;
}

static inline UINT64 ReadMsr(UINT32 msr)              { return __readmsr(msr); }
static inline VOID   WriteMsr(UINT32 msr, UINT64 val) { __writemsr(msr, val); }

static inline VOID Cpuid(UINT32 leaf, UINT32* eax, UINT32* ebx, UINT32* ecx, UINT32* edx) {
    INT32 r[4];
    __cpuid(r, (INT32)leaf);
    *eax = r[0]; *ebx = r[1]; *ecx = r[2]; *edx = r[3];
}
