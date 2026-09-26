# memchr and strlen for x86-64, System V ABI, GNU as (AT&T syntax).

    .text

# void *kb_memchr(const void *s, int c, size_t n)
    .globl  kb_memchr
    .type   kb_memchr, @function
kb_memchr:
    testq   %rdx, %rdx
    je      .Lnotfound
    movzbl  %sil, %esi
.Lloop:
    movzbl  (%rdi), %eax
    cmpl    %esi, %eax
    je      .Lfound
    incq    %rdi
    decq    %rdx
    jne     .Lloop
.Lnotfound:
    xorl    %eax, %eax
    ret
.Lfound:
    movq    %rdi, %rax
    ret
    .size   kb_memchr, .-kb_memchr

# size_t kb_strlen(const char *s)
    .globl  kb_strlen
    .type   kb_strlen, @function
kb_strlen:
    movq    %rdi, %rax
.Lscan:
    cmpb    $0, (%rax)
    je      .Ldone
    incq    %rax
    jmp     .Lscan
.Ldone:
    subq    %rdi, %rax
    ret
    .size   kb_strlen, .-kb_strlen

# int kb_sum(const int *v, size_t n)
    .globl  kb_sum
    .type   kb_sum, @function
kb_sum:
    xorl    %eax, %eax
    testq   %rsi, %rsi
    je      .Lsum_done
.Lsum_loop:
    addl    (%rdi), %eax
    addq    $4, %rdi
    decq    %rsi
    jne     .Lsum_loop
.Lsum_done:
    ret
    .size   kb_sum, .-kb_sum

    .section .note.GNU-stack,"",@progbits
