/* sjlj.S -- bionic sigsetjmp/siglongjmp on newlib setjmp/longjmp.
 *
 * Tail branches, not calls: setjmp must record the module's frame and
 * return address, so x30 has to arrive untouched. Signal masks do not exist
 * here, so the savemask argument is ignored. newlib's aarch64 jmp_buf (176
 * bytes) fits inside bionic's (256), and setjmp/longjmp themselves resolve to
 * newlib as well, so both halves agree on the buffer format.
 *
 * MIT licensed, see LICENSE.
 */
    .text
    .p2align 2

    .global bx_sigsetjmp
    .type   bx_sigsetjmp, %function
bx_sigsetjmp:
    b       setjmp
    .size   bx_sigsetjmp, .-bx_sigsetjmp

    .global bx_siglongjmp
    .type   bx_siglongjmp, %function
bx_siglongjmp:
    b       longjmp
    .size   bx_siglongjmp, .-bx_siglongjmp
