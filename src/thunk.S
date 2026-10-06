/* thunk.S - i686 tail-jump thunk. Only a4 (the stream) is ever swapped, and we
 * jmp rather than call, so the hooked function's calling convention never
 * matters -- its own ret N returns to its caller.
 */
    .text
    .globl  _PngLoaderThunk
    .def    _PngLoaderThunk; .scl 2; .type 32; .endef
_PngLoaderThunk:
    pushal
    pushl   0x2C(%esp)              /* a4 = [esp+0xC] on entry, +32 after pushal */
    call    _JxlMaybeSwapStream
    addl    $4, %esp
    testl   %eax, %eax
    je      1f
    movl    %eax, 0x2C(%esp)
1:
    popal
    jmp     *_g_trampEntry

    .globl  _PngLoaderThunkEnd
_PngLoaderThunkEnd:
