"""Run under WSL: assemble a small initrd from the matching Debian initrd.

All binaries/modules come from Debian. The installed disk remains unchanged.
"""
import pathlib, subprocess, shutil, tempfile, os, gzip
root = pathlib.Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='rvvm-initrd-') as temporary:
    work = pathlib.Path(temporary)
    extracted = work/'source'
    subprocess.run(['unmkinitramfs',str(root/'assets/debian-initrd.img'),str(extracted)],check=True)
    output = work/'root'
    for directory in ['bin','dev','proc','sys','newroot','usr/lib/riscv64-linux-gnu','lib']:
        (output/directory).mkdir(parents=True,exist_ok=True)
    source = extracted/'main'
    installer = work/'installer'
    installer.mkdir()
    subprocess.run(['cpio','-id','--no-absolute-filenames','*busybox','*lib/*'],cwd=installer,
                   input=gzip.decompress((root/'assets/initrd.gz').read_bytes()),
                   stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    busybox = next(p for p in installer.rglob('busybox') if not p.is_symlink())
    shutil.copy2(busybox,output/'bin/busybox')
    for applet in ['sh','mount','sleep','switch_root','cat']:
        (output/'bin'/applet).symlink_to('busybox')
    shutil.copy2(root/'build/guest-insmod',output/'bin/insmod')
    (output/'bin/insmod').chmod(0o755)
    # BusyBox is dynamically linked; copy only its required loader and C libs.
    for name in ['libc.so.6','libm.so.6','libresolv.so.2','ld-linux-riscv64-lp64d.so.1']:
        matches = [p for p in installer.rglob(name) if not p.is_symlink()]
        if matches:
            source_file = matches[0].resolve()
            shutil.copy2(source_file,output/'usr/lib/riscv64-linux-gnu'/name)
    loader = 'ld-linux-riscv64-lp64d.so.1'
    (output/'lib'/loader).symlink_to('../usr/lib/riscv64-linux-gnu/'+loader)
    source_modules = next((extracted/'early/usr/lib/modules').iterdir())
    metadata = source/'usr/lib/modules'/source_modules.name
    module_dir = output/'lib/modules'/source_modules.name
    module_dir.mkdir(parents=True)
    dependencies = {}
    for line in (metadata/'modules.dep').read_text().splitlines():
        name, deps = line.split(':',1)
        dependencies[name] = deps.split()
    chosen = set()
    ordered = []
    def include(name):
        if name in chosen: return
        chosen.add(name)
        for dep in dependencies[name]: include(dep)
        ordered.append(name)
    wanted = ['virtio_mmio','virtio_blk','virtio_net','crc32c_generic','ext4']
    for driver in wanted:
        for name in dependencies:
            if pathlib.Path(name).name.split('.')[0] == driver: include(name)
    for name in chosen:
        target = module_dir/name.removesuffix('.xz')
        target.parent.mkdir(parents=True,exist_ok=True)
        if name.endswith('.xz'):
            import lzma
            target.write_bytes(lzma.decompress((source_modules/name).read_bytes()))
        else: shutil.copy2(source_modules/name,target)
    # Text module metadata suffices for BusyBox modprobe, without host depmod.
    (module_dir/'modules.dep').write_text('\n'.join(
        name.removesuffix('.xz')+': '+' '.join(dep.removesuffix('.xz') for dep in dependencies[name])
        for name in sorted(chosen))+'\n')
    for name in ['modules.builtin','modules.order']:
        if (metadata/name).exists(): shutil.copy2(metadata/name,module_dir/name)
    init = output/'init'
    load_commands = '\n'.join('insmod /lib/modules/'+source_modules.name+'/'+name.removesuffix('.xz')+' || exec sh' for name in ordered)
    init.write_text('''#!/bin/sh
export PATH=/bin
mount -t devtmpfs devtmpfs /dev
mount -t proc proc /proc
mount -t sysfs sysfs /sys
''' + load_commands + '''
for attempt in 1 2 3 4 5; do
  [ -b /dev/vda1 ] && break
  sleep 1
done
mount -t ext4 -o rw /dev/vda1 /newroot || { echo 'RVVM: root mount failed'; exec sh; }
mount -o move /dev /newroot/dev
mount -o move /proc /newroot/proc
mount -o move /sys /newroot/sys
exec switch_root /newroot /sbin/init
''')
    init.chmod(0o755)
    paths = subprocess.check_output(['find','.','-print0'],cwd=output)
    destination = root/'assets/debian-initrd.cpio'
    with destination.open('wb') as file:
        subprocess.run(['cpio','--null','-o','-H','newc','--owner=0:0'],cwd=output,input=paths,stdout=file,check=True)
    print('Debian initrd:',destination.stat().st_size,'bytes; modules:',len(chosen))
