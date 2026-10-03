"""Run in WSL: build pinned upstream Linux benchmarks and a separate test initrd.

The measured results are host wall seconds for fixed workloads, not official
CoreMark/DMIPS scores: the guest uses an instruction-derived virtual clock.
"""
import hashlib, pathlib, shutil, subprocess, tempfile, urllib.request

root = pathlib.Path(__file__).resolve().parent.parent
sources = root / 'third_party/benchmarks'
busybox = sources / 'busybox-1.37.0.tar.bz2'
if not busybox.exists():
    busybox.write_bytes(urllib.request.urlopen('https://busybox.net/downloads/busybox-1.37.0.tar.bz2', timeout=90).read())
if hashlib.sha256(busybox.read_bytes()).hexdigest() != '3311dff32e746499f4df0d5df04d7eb396382d7e108bb9250e7b519b837043a4':
    raise RuntimeError('BusyBox source SHA-256 mismatch')
pins = {'coremark': '1f483d5b8316753a742cbf5590caf5bd0a4e4777',
        'riscv-tests': 'bcffa2b3188b040c611f90dc0b6e422f54775a09'}
for name, pin in pins.items():
    path = sources / name
    if not path.exists():
        subprocess.run(['git', 'clone', 'https://github.com/' +
                        ('eembc/coremark' if name == 'coremark' else 'riscv-software-src/riscv-tests'), str(path)], check=True)
    actual = subprocess.check_output(['git', '-C', str(path), 'rev-parse', 'HEAD'], text=True).strip()
    if actual != pin:
        raise RuntimeError(f'{name}: expected {pin}, found {actual}; preserve local changes and select the pinned revision')

out = root / 'build/guest-benchmarks'
out.mkdir(exist_ok=True)
cc = ['riscv64-linux-gnu-gcc', '-static', '-O2', '-march=rv64gc', '-mabi=lp64d']
core = sources / 'coremark'
for threads in (1, 4):
    subprocess.run(cc + ['-I'+str(core), '-I'+str(core/'posix'), '-DITERATIONS=10000',
                        '-DPERFORMANCE_RUN=1', '-DMULTITHREAD='+str(threads), '-DUSE_PTHREAD=1', '-pthread',
                        '-DFLAGS_STR="-static -O2 -march=rv64gc -mabi=lp64d -pthread"'] +
                   [str(core / name) for name in ['core_list_join.c', 'core_main.c', 'core_matrix.c',
                                                  'core_state.c', 'core_util.c', 'posix/core_portme.c']] +
                   ['-o', str(out / f'rvvm-coremark-{threads}')], check=True)

# Adapt only the bare-metal timer/entry and add final-value validation; the
# upstream hot loop/procedures remain intact. Preserve original sources.
dhry = sources / 'riscv-tests/benchmarks/dhrystone'
(out/'util.h').write_text('''#include <time.h>
#include <stdlib.h>
#define debug_printf printf
#define setStats(x) ((void)(x))
#undef NUMBER_OF_RUNS
#define NUMBER_OF_RUNS 1000000
#undef CLOCK_TYPE
#define CLOCK_TYPE "Linux CLOCK_MONOTONIC microseconds"
static inline long rvvm_ticks(void) {
  struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
  return t.tv_sec*1000000L + t.tv_nsec/1000;
}
#define read_csr(x) rvvm_ticks()
''')
main = (dhry/'dhrystone_main.c').read_text()
main = main.replace('Ptr_Comp:          %d', 'Ptr_Comp:          %ld')
main = main.replace('  return 0;', '''  int ok = Int_Glob == 5 && Bool_Glob == 1 && Ch_1_Glob == 'A' && Ch_2_Glob == 'B'
    && Arr_1_Glob[8] == 7 && Arr_2_Glob[8][7] == Number_Of_Runs + 10
    && Ptr_Glob->Discr == Ident_1 && Ptr_Glob->variant.var_1.Enum_Comp == Ident_3
    && Ptr_Glob->variant.var_1.Int_Comp == 17 && Next_Ptr_Glob->Discr == Ident_1
    && Next_Ptr_Glob->variant.var_1.Enum_Comp == Ident_2 && Next_Ptr_Glob->variant.var_1.Int_Comp == 18
    && Int_1_Loc == 5 && Int_2_Loc == 13 && Int_3_Loc == 7 && Enum_Loc == Ident_2
    && !strcmp(Str_1_Loc, "DHRYSTONE PROGRAM, 1'ST STRING")
    && !strcmp(Str_2_Loc, "DHRYSTONE PROGRAM, 2'ND STRING");
  printf("RVVM_DHRYSTONE_VALID=%d runs=%d\\n", ok, Number_Of_Runs);
  return !ok;''')
(out/'dhrystone_main.c').write_text(main)
subprocess.run(cc + ['-std=gnu89', '-Wno-implicit-int', '-Wno-implicit-function-declaration',
                    '-I'+str(dhry), '-I'+str(out), str(out/'dhrystone_main.c'), str(dhry/'dhrystone.c'),
                    '-o', str(out/'rvvm-dhrystone')], check=True)

with tempfile.TemporaryDirectory(prefix='rvvm-benchmark-initrd-') as directory:
    work = pathlib.Path(directory)
    subprocess.run(['cpio', '-id', '--no-absolute-filenames'], cwd=work,
                   input=(root/'assets/debian-performance-initrd.cpio').read_bytes(),
                   stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=True)
    install = ''
    for name in ['rvvm-coremark-1', 'rvvm-coremark-4', 'rvvm-dhrystone']:
        shutil.copy2(out/name, work/'bin'/name)
        (work/'bin'/name).chmod(0o755)
        install += f'/bin/busybox cp /bin/{name} /newroot/usr/local/bin/{name}\n/bin/busybox chmod 755 /newroot/usr/local/bin/{name}\n'
    shutil.copy2(root/'scripts/guest-benchmark.sh', work/'bin/rvvm-benchmark')
    (work/'bin/rvvm-benchmark').chmod(0o755)
    install += '/bin/busybox cp /bin/rvvm-benchmark /newroot/usr/local/bin/rvvm-benchmark\n/bin/busybox chmod 755 /newroot/usr/local/bin/rvvm-benchmark\n'
    shutil.copy2(busybox, work/'bin/rvvm-busybox-source.tar.bz2')
    install += '/bin/busybox mkdir -p /newroot/usr/local/share/rvvm\n/bin/busybox cp /bin/rvvm-busybox-source.tar.bz2 /newroot/usr/local/share/rvvm/busybox-1.37.0.tar.bz2\n'
    init = work/'init'
    init.write_text(init.read_text().replace('mount -o move /dev /newroot/dev', install+'\nmount -o move /dev /newroot/dev'))
    paths = subprocess.check_output(['find', '.', '-print0'], cwd=work)
    with (root/'build/debian-benchmark-initrd.cpio').open('wb') as file:
        subprocess.run(['cpio', '--null', '-o', '-H', 'newc', '--owner=0:0'], cwd=work, input=paths, stdout=file, check=True)
print('Created build/debian-benchmark-initrd.cpio; production initrd was preserved.')
