# SPDX-License-Identifier: MIT
#
# Stage 1 demo guest program: sums 1..10, stores the result to memory and
# stops with ECALL so that the trap can be inspected.
#
# Build with the LLVM cross toolchain (no WSL, no RISC-V SDK needed):
#
#   clang --target=riscv64-unknown-elf -march=rv64i -mabi=lp64 -nostdlib \
#         -Wl,-Ttext=0x80000000 -fuse-ld=lld -o program.elf program.S
#
#   nanorv run program.elf --trace

    .section .text
    .globl _start
_start:
    li      t0, 0            # accumulator
    li      t1, 0            # counter
    li      t2, 10           # limit
    li      t3, 0x80002000   # scratch memory

loop:
    addi    t1, t1, 1
    add     t0, t0, t1
    blt     t1, t2, loop

    sd      t0, 0(t3)        # publish the result

    li      a0, 0            # program exit code
    ecall                     # traps in stage 1; handled by the trap logic later

    # Not reached.
    j       .