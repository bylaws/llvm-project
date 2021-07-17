        .text

        .type   inner, @function
inner:
        .cfi_startproc
        int3
        ret
        .cfi_endproc
        .size   inner, .-inner

        .type   hotpatchable, @function
hotpatchable:
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        nop
        # Make the FDE entry start after the pad.
        .cfi_startproc
        push %rsp
        .cfi_def_cfa_offset 16
        call    inner
        add $16, %rsp
        .cfi_def_cfa_offset 8
        ret
        .cfi_endproc
        .size   hotpatchable, .-hotpatchable

        .globl  main
        .type   main, @function
main:
        .cfi_startproc
        call hotpatchable
        ret
        .cfi_endproc
        .size   main, .-main
