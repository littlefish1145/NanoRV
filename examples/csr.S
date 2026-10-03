# Zicsr demo guest program: reads and writes the unprivileged CSRs.
#
# Build:
#   .\build.ps1 -Source csr.S -Output csr.elf -March rv64i_zicsr
# Run:
#   rvvm run csr.elf --trace

    .section .text
    .globl _start
_start:
    li      t0, 0x2000
    csrs    mstatus, t0          # enable floating-point CSR access
    # --- identity registers (machine level) --------------------------------
    li      a0, 0
    csrr    t0, mvendorid
    csrr    t1, marchid           # 0x5256564d ("RVVM")
    csrr    t2, mimpid
    csrr    t3, mhartid           # 0 on the first hart

    # --- counters -----------------------------------------------------------
    csrr    s0, cycle
    csrr    s1, time
    csrr    s2, instret

    # --- frm / fflags / fcsr ----------------------------------------------
    li      t0, 7
    csrrw   t1, frm, t0           # t1 = old frm (0), frm = 7
    csrr    t2, fcsr              # t2 = 0xe0

    li      t0, 0x1f
    csrrs   s3, fflags, t0        # s3 = old fflags (0), fflags = 0x1f
    csrr    s4, fcsr              # s4 = 0xff

    csrrc   a1, fcsr, t0          # a1 = 0xff, clears the low five bits
    csrr    a2, fcsr              # a2 = 0xe0

    # The 5 bit immediate form cannot reach frm (fcsr bits [7:5]).
    csrrci  a3, fflags, 0x1f      # a3 = 0x00
    csrr    a4, fcsr

    # --- rd = x0 must not read, rs1 = x0 must not write --------------------
    li      t0, 0
    csrw    frm, t0               # rd = x0: writes 0 without reading
    csrr    a5, fcsr              # a5 = 0x00
    csrrs   a6, fcsr, x0          # rs1 = x0: a plain read, no write
    csrrc   a7, fcsr, x0          # same, no write

    # --- an illegal access traps ------------------------------------------
    # Not reached: the emulator stops on the trap below.
    li      a0, 1
    csrw    cycle, a0             # writing a read-only CSR is illegal

    ecall
