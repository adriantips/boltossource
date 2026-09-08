; ============================================================================
;  BoltOS  -  SYSCALL entry stub (IA32_LSTAR target).
;  On entry (from a ring-3 `syscall`): RCX = user RIP, R11 = user RFLAGS,
;  IF cleared by FMASK, and RSP is still the *user* stack. We switch to the
;  running process's kernel stack, marshal args to the SysV C ABI, dispatch,
;  then SYSRET back.
;
;  Interrupts are re-enabled for the body of the syscall. They have to be: the
;  network calls wait by spinning netif_poll_all() and hlt-ing until the next
;  timer tick, and with IF clear that tick never comes -- the hlt never wakes and
;  the whole machine stops dead rather than timing out. (That is why no ring-3
;  program could use the network: SYS_HTTPGET froze the machine too.)
;
;  Which means a syscall can now be preempted, so the user RSP can no longer be
;  stashed in a global: two threads in syscalls at once would overwrite each
;  other's and return to ring 3 on the wrong stack. It is pushed on the kernel
;  stack instead, one copy per invocation. The global is only a scratch slot
;  across the stack switch itself, where IF is still clear.
;
;  syscall ABI:  rax = number, rdi/rsi/rdx/r10/r8 = arg1..arg5, rax = return.
;  Per the syscall contract the kernel must preserve every register except
;  rax (return), rcx and r11 (clobbered by the SYSCALL/SYSRET hardware). The C
;  dispatcher is free to clobber the caller-saved registers, so we save the
;  user's rdi/rsi/rdx/r10/r8/r9 (callee-saved regs are preserved by C) and
;  restore them before returning to ring 3.
; ============================================================================
[BITS 64]

extern syscall_dispatch
extern syscall_kstack_top
extern syscall_user_rsp
global syscall_entry

section .text
syscall_entry:
    mov     [rel syscall_user_rsp], rsp     ; scratch across the switch (IF=0 here)
    mov     rsp, [rel syscall_kstack_top]   ; per-process kernel stack
    push    qword [rel syscall_user_rsp]    ; user RSP, now per-invocation
    push    rax                             ; padding: keeps the pushes even, so
                                            ; RSP stays 16-aligned for the C ABI
    push    rcx                             ; save user RIP
    push    r11                             ; save user RFLAGS
    push    rdi                             ; save user arg/temp registers that
    push    rsi                             ; the C dispatcher may clobber
    push    rdx
    push    r10
    push    r8
    push    r9

    sti                                     ; see the note above: a blocking
                                            ; syscall needs the timer to run

    ; (num=rax, a1=rdi, a2=rsi, a3=rdx, a4=r10, a5=r8)
    ;   -> C ABI dispatch(num, a1, a2, a3, a4, a5) in (rdi,rsi,rdx,rcx,r8,r9)
    mov     r9,  r8                         ; a5
    mov     r8,  r10                        ; a4
    mov     rcx, rdx                        ; a3
    mov     rdx, rsi                        ; a2
    mov     rsi, rdi                        ; a1
    mov     rdi, rax                        ; num
    call    syscall_dispatch                ; result in rax

    cli                                     ; back to the atomic SYSRET sequence
    pop     r9                              ; restore user registers
    pop     r8
    pop     r10
    pop     rdx
    pop     rsi
    pop     rdi
    pop     r11                             ; restore user RFLAGS
    pop     rcx                             ; restore user RIP
    add     rsp, 8                          ; drop the alignment padding
    pop     rsp                             ; restore user RSP from this stack
    o64 sysret                              ; 64-bit SYSRET -> ring 3
