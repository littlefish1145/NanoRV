#!/bin/sh
# Fixed workloads; report host wall time from debian_check.py, not guest scores.
set -eu
case "${1:-}" in
  coremark-1|coremark-4)
    threads=${1#coremark-}
    file=/tmp/rvvm-coremark-$threads.log
    # POSIX CoreMark uses SEED_ARG; ITERATIONS alone does not disable calibration.
    /usr/local/bin/rvvm-coremark-$threads 0 0 0x66 10000 > "$file"
    cat "$file"
    grep -q '2K performance run parameters' "$file"
    grep -Eq "^Iterations[[:space:]]*:[[:space:]]*$((threads * 10000))[[:space:]]*$" "$file"
    ! grep -Eq '\[[0-9]+\]ERROR|Cannot validate|ERROR.*data type' "$file"
    test "$(grep -c 'crclist.*0xe714' "$file")" = "$threads"
    test "$(grep -c 'crcmatrix.*0x1fd7' "$file")" = "$threads"
    test "$(grep -c 'crcstate.*0x8e3a' "$file")" = "$threads"
    echo "RVVM_COREMARK_CRC_OK threads=$threads iterations_per_thread=10000"
    ;;
  dhrystone)
    /usr/local/bin/rvvm-dhrystone
    ;;
  compression)
    # Deterministic 8 MiB mixed text/binary input, identical on both VMs.
    yes 'RVVM deterministic compression workload 0123456789 abcdefghijklmnopqrstuvwxyz' | head -c 4194304 > /tmp/rvvm-compression.input
    cat /usr/bin/bash >> /tmp/rvvm-compression.input
    dd if=/dev/zero bs=1M count=3 >> /tmp/rvvm-compression.input 2>/dev/null
    gzip -6 -c /tmp/rvvm-compression.input > /tmp/rvvm-compression.gz
    gzip -d -c /tmp/rvvm-compression.gz > /tmp/rvvm-compression.output
    cmp /tmp/rvvm-compression.input /tmp/rvvm-compression.output
    xz -3 -T1 -c /tmp/rvvm-compression.input > /tmp/rvvm-compression.xz
    xz -d -c /tmp/rvvm-compression.xz > /tmp/rvvm-compression.output
    cmp /tmp/rvvm-compression.input /tmp/rvvm-compression.output
    sha256sum /tmp/rvvm-compression.input
    echo RVVM_COMPRESSION_OK
    ;;
  7zip)
    # Fixed settings; internal duration calibration prevents equal-work comparison.
    seven=$(command -v 7zz || command -v 7z)
    "$seven" b 1 -mmt4 -md16
    echo RVVM_7ZIP_OK
    ;;
  busybox-compile)
    # Fresh source tree every invocation, bounded to /tmp, no reused objects.
    work=$(mktemp -d /tmp/rvvm-busybox.XXXXXX)
    trap 'rm -rf "$work"' EXIT
    tar -xjf /usr/local/share/rvvm/busybox-1.37.0.tar.bz2 -C "$work"
    cd "$work/busybox-1.37.0"
    make allnoconfig > "$work/config.log" 2>&1
    for option in STATIC ECHO CAT TRUE FALSE SHA256SUM; do
      sed -i "s/^# CONFIG_$option is not set$/CONFIG_$option=y/" .config
    done
    yes '' | make oldconfig >> "$work/config.log" 2>&1
    for option in STATIC ECHO CAT TRUE FALSE SHA256SUM; do
      grep -q "^CONFIG_$option=y$" .config
    done
    make -j4 > "$work/build.log" 2>&1 || { tail -n 50 "$work/build.log"; exit 1; }
    ./busybox echo RVVM_BUSYBOX_COMPILED
    test "$(printf abc | ./busybox sha256sum | cut -d' ' -f1)" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    sha256sum busybox
    echo RVVM_BUSYBOX_COMPILE_OK
    ;;
  *) echo 'Usage: rvvm-benchmark coremark-1|coremark-4|dhrystone|compression|7zip|busybox-compile' >&2; exit 2 ;;
esac
