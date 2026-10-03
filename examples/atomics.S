# RV64MA demo guest program: atomics and multiply/divide.
#
# Uses AMO to increment a memory counter, an LR/SC pair that only commits when
# the reservation survives, and the M extension. Stops with ECALL.
#
# Build:
#   .\build.ps1 -Source atomics.S -Output atomics.elf -March rv64ima
# Run:
#   rvvm run atomics.elf --trace

    .section .text
    .globl _start
_start:
    li      t0, 0x80004000      # counter
    li      t1, 0

    # --- AMO based increment ------------------------------------------------
    li      a0, 0               # rd
    li      a1, 1               # operand
    li      a2, 0x80004000       # rs1
    amoadd.d a0, a1, (a2)        # rd = old value, [0x80004000] += 1
    amoadd.d a0, a1, (a2)
    amoadd.d a0, a1, (a2)

    # --- AMO swap / xor / min ----------------------------------------------
    li      a1, 0
    amoswap.d a0, a1, (a2)      # clear the counter
    amoswap.d a0, a0, (a2)      # rd(old) = 0 -> a0 = 0

    li      a1, 0xffffffffffffffff
    amoswap.d a0, a1, (a2)      # counter = all ones
    li      a1, 0x7fff
    amoxor.d a0, a1, (a2)       # all ones ^ 0x7fff

    # --- LR / SC pair that succeeds ----------------------------------------
    li      a2, 0x80004000
    lr.d    a0, (a2)
    li      a1, 0x55
    sc.d    a0, a1, (a2)        # a0 == 0 on success

    # --- LR / SC pair that must fail ---------------------------------------
    lr.d    a0, (a2)
    li      a3, 0x80004008
    sd      zero, 0(a3)          # a store elsewhere keeps the reservation
    sc.d    a0, a1, (a2)        # a0 == 1: the reservation survived, so this commits

    # --- LR / SC pair broken by an interfering store -----------------------
    lr.d    a0, (a2)
    sd      zero, 0(a2)          # store to the reserved address
    li      a1, 0x66
    sc.d    a0, a1, (a2)        # a0 == 1: failed, and no store happened

    # --- M extension --------------------------------------------------------
    li      t0, 0xdeadbeef
    li      t1, 0x0000ffff
    mul     t2, t0, t1           # low word
    mulhu   t3, t0, t1           # high word, unsigned
    mulh    t4, t0, t1           # high word, signed
    divu    t5, t0, t1
    remu    t6, t0, t1

    li      s1, -100
    li      s2, 7
    div     s3, s1, s2           # -14 (truncated toward zero)
    rem     s4, s1, s2           # -2

    # divide by zero must not trap
    li      s5, 42
    div     s6, s5, zero         # -1
    divu    s7, s5, zero         # all ones
    rem     s8, s5, zero         # 42

    li      a0, 0
    ecall