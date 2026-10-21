#pragma once
#include <ntddk.h>

#pragma pack(push, 1)
struct VmcbControl {
    UINT16 intercept_cr_reads;
    UINT16 intercept_cr_writes;
    UINT16 intercept_dr_reads;
    UINT16 intercept_dr_writes;
    UINT32 intercept_exceptions;
    UINT32 intercept_misc1;
    UINT32 intercept_misc2;
    UINT32 intercept_misc3;
    UINT8  reserved0[0x03C - 0x018];
    UINT16 pause_filter_threshold;
    UINT16 pause_filter_count;
    UINT64 iopm_base_pa;
    UINT64 msrpm_base_pa;
    UINT64 tsc_offset;
    UINT32 guest_asid;
    UINT32 tlb_control;
    UINT64 v_intr;
    UINT64 interrupt_shadow;
    UINT64 exit_code;
    UINT64 exit_info1;
    UINT64 exit_info2;
    UINT64 exit_int_info;
    UINT64 np_enable;
    UINT64 avic_apic_bar;
    UINT64 ghcb_pa;
    UINT64 event_inject;
    UINT64 n_cr3;
    UINT64 lbr_virt_enable;
    UINT64 vmcb_clean;
    UINT64 nrip;
    UINT8  num_bytes_fetched;
    UINT8  guest_instruction_bytes[15];
    UINT64 avic_apic_backing_pa;
    UINT64 reserved1;
    UINT64 avic_logical_table_pa;
    UINT64 avic_physical_table_pa;
    UINT64 reserved2;
    UINT64 vmsa_pa;
    UINT8  reserved3[0x400 - 0x110];
};
static_assert(sizeof(VmcbControl) == 0x400);

struct VmcbSeg {
    UINT16 selector;
    UINT16 attrib;
    UINT32 limit;
    UINT64 base;
};

struct VmcbSave {
    VmcbSeg es;
    VmcbSeg cs;
    VmcbSeg ss;
    VmcbSeg ds;
    VmcbSeg fs;
    VmcbSeg gs;
    VmcbSeg gdtr;
    VmcbSeg ldtr;
    VmcbSeg idtr;
    VmcbSeg tr;
    UINT8   reserved0[0x4CB - 0x4A0];
    UINT8   cpl;
    UINT32  reserved1;
    UINT64  efer;
    UINT8   reserved2[0x548 - 0x4D8];
    UINT64  cr4;
    UINT64  cr3;
    UINT64  cr0;
    UINT64  dr7;
    UINT64  dr6;
    UINT64  rflags;
    UINT64  rip;
    UINT8   reserved3[0x5D8 - 0x580];
    UINT64  rsp;
    UINT64  s_cet;
    UINT64  ssp;
    UINT64  isst_addr;
    UINT64  rax;
    UINT64  star;
    UINT64  lstar;
    UINT64  cstar;
    UINT64  sfmask;
    UINT64  kernel_gs_base;
    UINT64  sysenter_cs;
    UINT64  sysenter_esp;
    UINT64  sysenter_eip;
    UINT64  cr2;
    UINT8   reserved4[0x668 - 0x648];
    UINT64  g_pat;
    UINT64  dbgctl;
    UINT64  br_from;
    UINT64  br_to;
    UINT64  last_excp_from;
    UINT8   reserved5[0x6A0 - 0x690];
    UINT64  spec_ctrl;
};

struct Vmcb {
    VmcbControl control;
    VmcbSave    save;
    UINT8       padding[0x1000 - sizeof(VmcbControl) - sizeof(VmcbSave)];
};
static_assert(sizeof(Vmcb) <= 0x1000);
#pragma pack(pop)

#define INTERCEPT_INTR         (1u << 0)
#define INTERCEPT_NMI          (1u << 1)
#define INTERCEPT_CPUID        (1u << 18)
#define INTERCEPT_HLT          (1u << 24)
#define INTERCEPT_IOIO         (1u << 27)
#define INTERCEPT_MSR          (1u << 28)
#define INTERCEPT_VMRUN        (1u << 0)
#define INTERCEPT_VMMCALL      (1u << 1)

#define NP_ENABLE              (1ULL << 0)

#define TLB_CONTROL_DO_NOTHING  0
#define TLB_CONTROL_FLUSH_ALL   1
#define TLB_CONTROL_FLUSH_GUEST 3

#define VMCB_CLEAN_INTERCEPTS  (1u << 0)
#define VMCB_CLEAN_IOPM        (1u << 1)
#define VMCB_CLEAN_ASID        (1u << 2)
#define VMCB_CLEAN_NP          (1u << 4)
#define VMCB_CLEAN_CR          (1u << 5)
#define VMCB_CLEAN_DR          (1u << 6)
#define VMCB_CLEAN_DT          (1u << 7)
#define VMCB_CLEAN_SEG         (1u << 8)
#define VMCB_CLEAN_ALL         0xFFFFFFFF

#define VMEXIT_CPUID           0x72
#define VMEXIT_HLT             0x78
#define VMEXIT_MSR             0x7C
#define VMEXIT_VMRUN           0x80
#define VMEXIT_VMMCALL         0x81
#define VMEXIT_NPF             0x400
#define VMEXIT_INVALID         0xFFFFFFFFFFFFFFFFULL

#define MSR_VM_CR              0xC0010114
#define MSR_VM_HSAVE_PA        0xC0010117
#define MSR_EFER               0xC0000080
#define MSR_LSTAR              0xC0000082
#define MSR_STAR               0xC0000081
#define MSR_CSTAR              0xC0000083
#define MSR_SFMASK             0xC0000084
#define MSR_FS_BASE            0xC0000100
#define MSR_GS_BASE            0xC0000101
#define MSR_KERNEL_GS_BASE     0xC0000102
#define MSR_PAT                0x00000277
#define EFER_SVME              (1ULL << 12)
