#pragma once
#include <ntddk.h>
#include <intrin.h>
#include "vmcb.h"

#define HV_POOL_TAG 'pVMA'
#define MAX_PROCESSORS 64

// MSRPM: 8KB bitmap, 2 bits per MSR (read | write)
// Covers 3 ranges: 0x00000000–0x1FFF, 0xC0000000–0xC0001FFF, 0xC0010000–0xC0011FFF
#define MSRPM_SIZE (8192)

// IOPM: 12KB, 1 bit per I/O port
#define IOPM_SIZE  (12288)

// NPT pre-alloc pool size (pages)
#define NPT_POOL_PAGES 256

// GuestRegs: matches our asm stub push order (r15 first, rcx last)
// NEVER cast this to any other struct — field names are the contract
#pragma pack(push, 1)
struct GuestRegs {
    UINT64 r15;  // offset 0x00 (pushed first = lowest addr)
    UINT64 r14;  // offset 0x08
    UINT64 r13;  // offset 0x10
    UINT64 r12;  // offset 0x18
    UINT64 r11;  // offset 0x20
    UINT64 r10;  // offset 0x28
    UINT64 r9;   // offset 0x30
    UINT64 r8;   // offset 0x38
    UINT64 rdi;  // offset 0x40
    UINT64 rsi;  // offset 0x48
    UINT64 rbp;  // offset 0x50
    UINT64 rbx;  // offset 0x58
    UINT64 rdx;  // offset 0x60
    UINT64 rcx;  // offset 0x68 (pushed last = highest addr)
};
static_assert(sizeof(GuestRegs) == 14 * 8, "GuestRegs size mismatch");
#pragma pack(pop)

// Per-logical-processor hypervisor state
struct PerCoreData {
    Vmcb*    vmcb;          // guest VMCB (4KB aligned)
    Vmcb*    host_vmcb;     // host state save area
    UINT64   vmcb_pa;       // physical address of guest VMCB
    UINT64   host_vmcb_pa;  // physical address of host VMCB
    UINT64   host_stack[512]; // host RSP when exit handler runs
    UINT8*   msrpm;         // MSRPM bitmap (8KB, 4KB aligned)
    UINT8*   iopm;          // IOPM bitmap (12KB, 4KB aligned)
    UINT64   msrpm_pa;
    UINT64   iopm_pa;
    BOOLEAN  virtualized;   // true after VMRUN
    UINT32   core_id;       // logical processor index
};

// Global hypervisor state
struct HvState {
    PerCoreData* cores[MAX_PROCESSORS];
    UINT32       num_cores;
    UINT64       npt_root_pa;     // shared NPT root (same for all cores)
    VOID*        npt_root_va;
    BOOLEAN      npt_initialized;

    // Pre-allocated NPT page pool (GIF=0 safe)
    VOID*        npt_pool[NPT_POOL_PAGES];
    UINT64       npt_pool_pa[NPT_POOL_PAGES];
    volatile LONG npt_pool_head;  // atomic index

    // SIPI delivery state (per-core, BSP writes, AP reads)
    volatile LONG sipi_pending[MAX_PROCESSORS];
    volatile UCHAR sipi_vector[MAX_PROCESSORS];
    volatile LONG init_received[MAX_PROCESSORS];
};

extern HvState g_hv;

// Physical/virtual address helpers
static inline UINT64 VaToPA(VOID* va) {
    return MmGetPhysicalAddress(va).QuadPart;
}

static inline VOID* AllocContiguous(SIZE_T size) {
    PHYSICAL_ADDRESS lo = {}, hi = {};
    hi.QuadPart = 0xFFFFFFFFFFFFFFFFLL;
    return MmAllocateContiguousMemory(size, hi);
}

// MSR helpers (safe wrappers around intrinsics)
static inline UINT64 ReadMsr(UINT32 msr) { return __readmsr(msr); }
static inline VOID   WriteMsr(UINT32 msr, UINT64 val) { __writemsr(msr, val); }

// Cpuid helper
static inline VOID Cpuid(UINT32 leaf, UINT32* eax, UINT32* ebx, UINT32* ecx, UINT32* edx) {
    INT32 regs[4];
    __cpuid(regs, (INT32)leaf);
    *eax = (UINT32)regs[0]; *ebx = (UINT32)regs[1];
    *ecx = (UINT32)regs[2]; *edx = (UINT32)regs[3];
}

// DbgPrint wrapper — safe to call from non-GIF=0 contexts only
#define HV_LOG(fmt, ...) DbgPrintEx(DPFLTR_IHVDRIVER_ID, DPFLTR_ERROR_LEVEL, "[HV] " fmt "\n", ##__VA_ARGS__)
