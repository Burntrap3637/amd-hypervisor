.CODE

extern SvmExitHandler : proc

SvmVmrun PROC
    mov rsp, rdx
    push rbx
    mov rbx, rcx

    mov rax, rbx
    vmload rax

vmrun_loop:
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
    push rbx
    push rdx
    push rcx

    mov rax, rbx
    vmrun rax

    mov rcx, rsp
    xor rdx, rdx
    sub rsp, 20h
    call SvmExitHandler
    add rsp, 20h

    test rax, rax
    jnz vmrun_done

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
    add rsp, 14*8
    pop rbx
    ret
SvmVmrun ENDP


SvmVmsave PROC
    mov rax, rcx
    vmsave rax
    ret
SvmVmsave ENDP


SvmVmload PROC
    mov rax, rcx
    vmload rax
    ret
SvmVmload ENDP


SvmStgi PROC
    stgi
    ret
SvmStgi ENDP


SvmClgi PROC
    clgi
    ret
SvmClgi ENDP


END
