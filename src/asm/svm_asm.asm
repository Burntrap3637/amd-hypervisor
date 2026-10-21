; AMD SVM assembly stubs
; Assembled with MASM (ml64.exe) — WDK build system handles this

.CODE

; SvmVmrun(vmcb_pa: UINT64, host_stack_top: VOID*)
;
; Calling convention: RCX = vmcb_pa, RDX = host_stack_top
;
; This is the blue-pill trampoline.
; 1. Switch to host stack
; 2. Save all GPRs that the exit handler needs (push order = GuestRegs layout: r15 first, rcx last)
; 3. VMRUN (RAX = VMCB physical address)
; 4. On #VMEXIT, control returns here
; 5. Call SvmExitHandler(regs, core) — we need core ptr too, passed via thread-local trick
; 6. Restore GPRs, return to loop in SvmVirtualizeCore

; Note: RCX/RDX are caller-saved (fastcall/Microsoft ABI) but we save them too because
; the exit handler needs to inspect/modify the full guest register state.
; The push order MUST match struct GuestRegs exactly (r15 at offset 0, rcx at offset 0x68).

extern SvmExitHandler : proc

SvmVmrun PROC
    ; rcx = vmcb_pa, rdx = host_stack_top
    ; Switch to host stack
    mov rsp, rdx

    ; Save VMCB PA in RAX for VMRUN; save original rcx in a callee-saved reg
    ; We keep vmcb_pa in rbx across exits (rbx is callee-saved, we save/restore it)
    push rbx
    mov rbx, rcx        ; rbx = vmcb_pa (survives VMRUN)

    ; VMLOAD — loads FS/GS/LDTR/STAR/SYSENTER etc. from VMCB save area
    ; This primes the guest hidden state that VMRUN needs
    mov rax, rbx
    vmload rax

vmrun_loop:
    ; Push guest GPRs in GuestRegs order: r15 first, rcx last
    ; (caller's RCX/RDX etc. at this point are guest values from VMCB — they were
    ;  set up by SvmInitCore from the captured CPU state)
    push r15
    push r14
    push r13
    push r12
    push r11
    push r10
    push r9
    push r8
    push rdi
    push rsi
    push rbp
    push rbx    ; pushed as placeholder; exit handler gets real rbx via VMCB
    push rdx
    push rcx

    ; RAX = VMCB physical address for VMRUN
    mov rax, rbx

    ; RCX on stack (just pushed) is the guest RCX; we need the actual guest RAX
    ; from VMCB.save.rax — VMRUN sources RAX from that field, not from register
    ; So before VMRUN, load guest RAX from the VMCB rax field into RAX:
    ; (VMCB.save.rax is at offset 0x5F8, but we access via VMCB base + offset)
    ; For simplicity, we keep RAX = vmcb_pa throughout; VMRUN will overwrite with guest RAX.
    ; The guest RAX is already in VMCB.save.rax from our initial setup.

    vmrun rax       ; executes guest; returns here on #VMEXIT
                    ; after VMEXIT: VMCB.save.rax = guest RAX at exit

    ; On exit: RSP still points at our GuestRegs frame
    ; Pass regs ptr (RSP) and core ptr to exit handler
    ; core ptr: stored in r10 slot for now — actually we pass per-core data
    ; via a per-processor variable; SvmExitHandler finds it via KeGetCurrentProcessorIndex

    ; RCX = &GuestRegs (first arg), RDX = NULL (core looked up inside C handler)
    mov rcx, rsp    ; ptr to GuestRegs frame on stack
    xor rdx, rdx    ; core ptr resolved in C via KeGetCurrentProcessorIndex

    ; Shadow space for callee (Microsoft x64 ABI requires 32 bytes)
    sub rsp, 20h
    call SvmExitHandler
    add rsp, 20h

    ; Check return value: if SvmExitHandler returns 0 = continue, non-zero = stop
    test rax, rax
    jnz vmrun_done

    ; Restore GPRs from the (possibly modified) GuestRegs frame
    ; Order: pop rcx, rdx, rbx, rbp, rsi, rdi, r8..r15
    pop rcx
    pop rdx
    pop rbx
    pop rbp
    pop rsi
    pop rdi
    pop r8
    pop r9
    pop r10
    pop r11
    pop r12
    pop r13
    pop r14
    pop r15

    jmp vmrun_loop

vmrun_done:
    ; Devirtualization: restore stack and return to SvmVirtualizeCore caller
    ; Guest RSP was written back to VMCB by SvmExitHandler; the C code will
    ; adjust host stack; we just need to pop our frame and rbx
    add rsp, 14*8   ; discard GuestRegs frame (14 pushes * 8 bytes)
    pop rbx
    ret
SvmVmrun ENDP


; SvmVmsave(vmcb_pa: UINT64)
; rcx = vmcb_pa
SvmVmsave PROC
    mov rax, rcx
    vmsave rax
    ret
SvmVmsave ENDP


; SvmVmload(vmcb_pa: UINT64)
; rcx = vmcb_pa
SvmVmload PROC
    mov rax, rcx
    vmload rax
    ret
SvmVmload ENDP


; SvmStgi — set global interrupt flag
SvmStgi PROC
    stgi
    ret
SvmStgi ENDP


; SvmClgi — clear global interrupt flag
SvmClgi PROC
    clgi
    ret
SvmClgi ENDP


END
