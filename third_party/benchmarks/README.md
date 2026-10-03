# Guest benchmark sources

Only used by scripts/prepare-benchmarks.py; not linked into RVVM.

| Source | Pinned revision / digest | License |
|---|---|---|
| https://github.com/eembc/coremark | 1f483d5b8316753a742cbf5590caf5bd0a4e4777 | Apache-2.0, original LICENSE/source notices retained |
| https://github.com/riscv-software-src/riscv-tests | bcffa2b3188b040c611f90dc0b6e422f54775a09 | Original LICENSE and Dhrystone notices retained |
| https://busybox.net/downloads/busybox-1.37.0.tar.bz2 | SHA-256 3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4 | GPL-2.0, complete source archive including LICENSE retained |

CoreMark algorithm sources are unchanged. A separate generated Dhrystone Linux
entry/timer shim adds output validation; the original source is preserved.
Host wall-clock fixed workloads are reported, never official CoreMark/DMIPS scores.
