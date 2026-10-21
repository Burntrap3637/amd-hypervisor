#pragma once
#include <ntddk.h>

// VMCB control area — offsets 0x000–0x3FF
// AMD APM Vol 2, Section 15.11
#pragma pack(push, 1)
struct VmcbControl {
    // Intercept vectors
    UINT16 intercept_cr_reads;      // 0x000 CR0–15 read intercepts
    UINT16 intercept_cr_writes;     // 0x002 CR0–15 write intercepts
    UINT16 intercept_dr_reads;      // 0x004 DR0–15 read intercepts
    UINT16 intercept_dr_writes;     // 0x006 DR0–15 write intercepts
    UINT32 intercept_exceptions;    // 0x008 exception intercepts (bit per vector)
    UINT32 intercept_misc1;         // 0x00C misc intercepts set 1
    UINT32 intercept_misc2;         // 0x010 misc intercepts set 2
    UINT32 intercept_misc3;         // 0x014
    UINT8  reserved0[0x03C - 0x018]; // 0x018–0x03B
    UINT16 pause_filter_threshold;  // 0x03C
    UINT16 pause_filter_count;      // 0x03E
    UINT64 iopm_base_pa;            // 0x040 physical addr of IOPM
    UINT64 msrpm_base_pa;           // 0x048 physical addr of MSRPM
    UINT64 tsc_offset;              // 0x050 added to guest RDTSC result
    UINT32 guest_asid;              // 0x058 address space ID (1+ for guests)
    UINT32 tlb_control;             // 0x05C TLB flush control
    UINT64 v_intr;                  // 0x060 virtual interrupt control
    UINT64 interrupt_shadow;        // 0x068
    UINT64 exit_code;               // 0x070 #VMEXIT reason
    UINT64 exit_info1;              // 0x078 exit qualification 1
    UINT64 exit_info2;              // 0x080 exit qualification 2
    UINT64 exit_int_info;           // 0x088 interrupted event info
    UINT64 np_enable;               // 0x090 nested paging enable (bit 0)
    UINT64 avic_apic_bar;           // 0x098
    UINT64 ghcb_pa;                 // 0x0A0
    UINT64 event_inject;            // 0x0A8 event to inject on VMRUN
    UINT64 n_cr3;                   // 0x0B0 nested page table root PA
    UINT64 lbr_virt_enable;         // 0x0B8
    UINT64 vmcb_clean;              // 0x0C0 VMCB clean bits
    UINT64 nrip;                    // 0x0C8 next sequential RIP (AMD only)
    UINT8  num_bytes_fetched;       // 0x0D0 decode assist: bytes fetched
    UINT8  guest_instruction_bytes[15]; // 0x0D1 fetched instruction bytes
    UINT64 avic_apic_backing_pa;    // 0x0E0
    UINT64 reserved1;               // 0x0E8
    UINT64 avic_logical_table_pa;   // 0x0F0
    UINT64 avic_physical_table_pa;  // 0x0F8
    UINT64 reserved2;               // 0x100
    UINT64 vmsa_pa;                 // 0x108 (SEV-ES)
    UINT8  reserved3[0x400 - 0x110]; // pad to save area
};
static_assert(sizeof(VmcbControl) == 0x400, "VmcbControl must be 0x400 bytes");

// Segment descriptor as stored in VMCB save area
struct VmcbSeg {
    UINT16 selector;
    UINT16 attrib;   // packed: type[3:0] s[4] dpl[6:5] p[7] avl[8] l[9] db[10] g[11]
    UINT32 limit;
    UINT64 base;
};

// VMCB save state area — offsets 0x400+
struct VmcbSave {
    VmcbSeg es;         // 0x400
    VmcbSeg cs;         // 0x410
    VmcbSeg ss;         // 0x420
    VmcbSeg ds;         // 0x430
    VmcbSeg fs;         // 0x440
    VmcbSeg gs;         // 0x450
    VmcbSeg gdtr;       // 0x460
    VmcbSeg ldtr;       // 0x470
    VmcbSeg idtr;       // 0x480
    VmcbSeg tr;         // 0x490
    UINT8   reserved0[0x4CB - 0x4A0];
    UINT8   cpl;        // 0x4CB current privilege level
    UINT32  reserved1;
    UINT64  efer;       // 0x4D0
    UINT8   reserved2[0x548 - 0x4D8];
    UINT64  cr4;        // 0x548
    UINT64  cr3;        // 0x550
    UINT64  cr0;        // 0x558
    UINT64  dr7;        // 0x560
    UINT64  dr6;        // 0x568
    UINT64  rflags;     // 0x570
    UINT64  rip;        // 0x578
    UINT8   reserved3[0x5D8 - 0x580];
    UINT64  rsp;        // 0x5D8
    UINT64  s_cet;      // 0x5E0
    UINT64  ssp;        // 0x5E8
    UINT64  isst_addr;  // 0x5F0
    UINT64  rax;        // 0x5F8
    UINT64  star;       // 0x600
    UINT64  lstar;      // 0x608
    UINT64  cstar;      // 0x610
    UINT64  sfmask;     // 0x618
    UINT64  kernel_gs_base; // 0x620
    UINT64  sysenter_cs;    // 0x628
    UINT64  sysenter_esp;   // 0x630
    UINT64  sysenter_eip;   // 0x638
    UINT64  cr2;        // 0x640
    UINT8   reserved4[0x668 - 0x648];
    UINT64  g_pat;      // 0x668
    UINT64  dbgctl;     // 0x670
    UINT64  br_from;    // 0x678
    UINT64  br_to;      // 0x680
    UINT64  last_excp_from; // 0x688
    UINT8   reserved5[0x6A0 - 0x690];
    UINT64  spec_ctrl;  // 0x6A0
};

// Full VMCB — must be 4KB aligned, 4KB size
struct Vmcb {
    VmcbControl control; // 0x000–0x3FF
    VmcbSave    save;    // 0x400–0x6FF
    UINT8       padding[0x1000 - sizeof(VmcbControl) - sizeof(VmcbSave)];
};
static_assert(sizeof(Vmcb) <= 0x1000, "Vmcb must fit in 4KB");
#pragma pack(pop)

// intercept_misc1 bits (offset 0x00C)
#define INTERCEPT_INTR         (1u << 0)
#define INTERCEPT_NMI          (1u << 1)
#define INTERCEPT_SMI          (1u << 2)
#define INTERCEPT_CPUID        (1u << 18)
#define INTERCEPT_HLT          (1u << 24)
#define INTERCEPT_IOIO         (1u << 27)
#define INTERCEPT_MSR          (1u << 28)

// intercept_misc2 bits (offset 0x010)
#define INTERCEPT_VMRUN        (1u << 0)
#define INTERCEPT_VMMCALL      (1u << 1)
#define INTERCEPT_VMLOAD       (1u << 2)
#define INTERCEPT_VMSAVE       (1u << 3)
#define INTERCEPT_STGI         (1u << 4)
#define INTERCEPT_CLGI         (1u << 5)
#define INTERCEPT_SKINIT       (1u << 6)
#define INTERCEPT_RDTSCP       (1u << 8)
#define INTERCEPT_MONITOR      (1u << 10)
#define INTERCEPT_MWAIT        (1u << 11)

// np_enable bits (offset 0x090)
#define NP_ENABLE              (1ULL << 0)

// tlb_control values
#define TLB_CONTROL_DO_NOTHING  0
#define TLB_CONTROL_FLUSH_ALL   1
#define TLB_CONTROL_FLUSH_GUEST 3

// vmcb_clean bits — set to skip reloading cached state
#define VMCB_CLEAN_INTERCEPTS  (1u << 0)
#define VMCB_CLEAN_IOPM        (1u << 1)
#define VMCB_CLEAN_ASID        (1u << 2)
#define VMCB_CLEAN_TPR         (1u << 3)
#define VMCB_CLEAN_NP          (1u << 4)
#define VMCB_CLEAN_CR          (1u << 5)
#define VMCB_CLEAN_DR          (1u << 6)
#define VMCB_CLEAN_DT          (1u << 7)
#define VMCB_CLEAN_SEG         (1u << 8)
#define VMCB_CLEAN_CR2         (1u << 9)
#define VMCB_CLEAN_LBR         (1u << 10)
#define VMCB_CLEAN_AVIC        (1u << 11)
#define VMCB_CLEAN_ALL         0xFFFFFFFF

// #VMEXIT codes
#define VMEXIT_CPUID           0x72
#define VMEXIT_HLT             0x78
#define VMEXIT_IOIO            0x7B
#define VMEXIT_MSR             0x7C
#define VMEXIT_VMRUN           0x80
#define VMEXIT_VMMCALL         0x81
#define VMEXIT_VMLOAD          0x82
#define VMEXIT_VMSAVE          0x83
#define VMEXIT_STGI            0x84
#define VMEXIT_CLGI            0x85
#define VMEXIT_SKINIT          0x86
#define VMEXIT_RDTSCP          0x87
#define VMEXIT_WBINVD          0x89
#define VMEXIT_MONITOR         0x8A
#define VMEXIT_MWAIT           0x8B
#define VMEXIT_NPF             0x400  // nested page fault
#define VMEXIT_INVALID         0xFFFFFFFFFFFFFFFFULL

// MSRs
#define MSR_VM_CR              0xC0010114
#define MSR_VM_HSAVE_PA        0xC0010117
#define MSR_EFER               0xC0000080
#define MSR_LSTAR              0xC0000082
#define MSR_STAR               0xC0000081
#define MSR_SFMASK             0xC0000084
#define MSR_FS_BASE            0xC0000100
#define MSR_GS_BASE            0xC0000101
#define MSR_KERNEL_GS_BASE     0xC0000102
#define MSR_TSC                0x00000010
#define MSR_PAT                0x00000277
#define EFER_SVME              (1ULL << 12)
