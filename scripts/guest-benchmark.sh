#!/bin/sh
# Fixed workloads; report host wall time from debian_check.py, not guest scores.
set -eu
case "${1:-}" in
  coremark-1|coremark-4)
    threads=${1#coremark-}
    file=/tmp/nanorv-coremark-$threads.log
    # POSIX CoreMark uses SEED_ARG; ITERATIONS alone does not disable calibration.
    /usr/local/bin/nanorv-coremark-$threads 0 0 0x66 10000 > "$file"
    cat "$file"
    grep -q '2K performance run parameters' "$file"
    grep -Eq "^Iterations[[:space:]]*:[[:space:]]*$((threads * 10000))[[:space:]]*$" "$file"
    ! grep -Eq '\[[0-9]+\]ERROR|Cannot validate|ERROR.*data type' "$file"
    test "$(grep -c 'crclist.*0xe714' "$file")" = "$threads"
    test "$(grep -c 'crcmatrix.*0x1fd7' "$file")" = "$threads"
    test "$(grep -c 'crcstate.*0x8e3a' "$file")" = "$threads"
    echo "NanoRV_COREMARK_CRC_OK threads=$threads iterations_per_thread=10000"
    ;;
  dhrystone)
    /usr/local/bin/nanorv-dhrystone
    ;;
  compression)
    # Deterministic 8 MiB mixed text/binary input, identical on both VMs.
    yes 'NanoRV deterministic compression workload 0123456789 abcdefghijklmnopqrstuvwxyz' | head -c 4194304 > /tmp/nanorv-compression.input
    cat /usr/bin/bash >> /tmp/nanorv-compression.input
    dd if=/dev/zero bs=1M count=3 >> /tmp/nanorv-compression.input 2>/dev/null
    gzip -6 -c /tmp/nanorv-compression.input > /tmp/nanorv-compression.gz
    gzip -d -c /tmp/nanorv-compression.gz > /tmp/nanorv-compression.output
    cmp /tmp/nanorv-compression.input /tmp/nanorv-compression.output
    xz -3 -T1 -c /tmp/nanorv-compression.input > /tmp/nanorv-compression.xz
    xz -d -c /tmp/nanorv-compression.xz > /tmp/nanorv-compression.output
    cmp /tmp/nanorv-compression.input /tmp/nanorv-compression.output
    sha256sum /tmp/nanorv-compression.input
    echo NanoRV_COMPRESSION_OK
    ;;
  7zip)
    # Fixed settings; internal duration calibration prevents equal-work comparison.
    seven=$(command -v 7zz || command -v 7z)
    "$seven" b 1 -mmt4 -md16
    echo NanoRV_7ZIP_OK
    ;;
  busybox-compile)
    # Fresh source tree every invocation, bounded to /tmp, no reused objects.
    work=$(mktemp -d /tmp/nanorv-busybox.XXXXXX)
    trap 'rm -rf "$work"' EXIT
    tar -xjf /usr/local/share/nanorv/busybox-1.37.0.tar.bz2 -C "$work"
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
    ./busybox echo NanoRV_BUSYBOX_COMPILED
    test "$(printf abc | ./busybox sha256sum | cut -d' ' -f1)" = ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
    sha256sum busybox
    echo NanoRV_BUSYBOX_COMPILE_OK
    ;;
  *) echo 'Usage: nanorv-benchmark coremark-1|coremark-4|dhrystone|compression|7zip|busybox-compile' >&2; exit 2 ;;
esac
