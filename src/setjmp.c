/* x86-64 SysV setjmp/longjmp for a payload with no libc.
 *
 * clown68000 unwinds 68000 exceptions -- address errors, illegal
 * instructions, TRAP -- by longjmp'ing out of the middle of the interpreter,
 * so this is on the correctness path for any game that takes an exception,
 * not just an optional nicety.
 *
 * Only the callee-saved registers, the stack pointer and the return address
 * need preserving: the SysV calling convention already lets a call clobber
 * everything else, so the caller cannot be relying on it.
 *
 * jmp_buf layout, all 8 bytes wide:
 *   0  rbx   8  rbp   16 r12   24 r13
 *   32 r14  40 r15    48 rsp (as it will be after ret)   56 return address
 *
 * No MXCSR or x87 control word is saved. Nothing in this payload changes
 * either -- there is no floating point in the emulation path at all, since the
 * FM and PSG tables that would have needed libm are compiled in as constants
 * -- so restoring them would be restoring values that never varied.
 */

#include <setjmp.h>

__attribute__((naked, returns_twice))
int setjmp(jmp_buf env) {
    __asm__ volatile (
        "movq %rbx,  0(%rdi)\n\t"
        "movq %rbp,  8(%rdi)\n\t"
        "movq %r12, 16(%rdi)\n\t"
        "movq %r13, 24(%rdi)\n\t"
        "movq %r14, 32(%rdi)\n\t"
        "movq %r15, 40(%rdi)\n\t"
        /* The stack pointer to restore is the one the caller will see after
           this function returns, i.e. past the pushed return address. */
        "leaq 8(%rsp), %rax\n\t"
        "movq %rax, 48(%rdi)\n\t"
        "movq (%rsp), %rax\n\t"
        "movq %rax, 56(%rdi)\n\t"
        "xorl %eax, %eax\n\t"
        "ret"
    );
}

__attribute__((naked, noreturn))
void longjmp(jmp_buf env, int val) {
    __asm__ volatile (
        /* setjmp must never appear to return 0 a second time, so a longjmp
           asking for 0 is promoted to 1 -- the caller distinguishes the two
           paths by exactly that. */
        "movl %esi, %eax\n\t"
        "testl %eax, %eax\n\t"
        "jnz 1f\n\t"
        "movl $1, %eax\n"
        "1:\n\t"
        "movq  0(%rdi), %rbx\n\t"
        "movq  8(%rdi), %rbp\n\t"
        "movq 16(%rdi), %r12\n\t"
        "movq 24(%rdi), %r13\n\t"
        "movq 32(%rdi), %r14\n\t"
        "movq 40(%rdi), %r15\n\t"
        "movq 48(%rdi), %rsp\n\t"
        /* Jump rather than push-and-ret: the saved rsp is already correct and
           pushing would put the address below it, where a signal could land. */
        "jmp *56(%rdi)"
    );
}
