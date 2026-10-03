"""One-time, offline image preparation under WSL; never replaces a working disk."""
import pathlib, tarfile, shutil, tempfile, subprocess
root = pathlib.Path(__file__).resolve().parent.parent
assets = root/'assets'
disk = assets/'disk.raw'
if not disk.exists():
    temporary = assets/'disk.raw.preparing'
    try:
        with tarfile.open(assets/'debian-13-nocloud-riscv64.tar.xz','r:xz') as archive:
            member = next(m for m in archive if pathlib.PurePosixPath(m.name).name == 'disk.raw' and m.isfile())
            with archive.extractfile(member) as source, temporary.open('wb') as output:
                shutil.copyfileobj(source,output,1024*1024)
        temporary.rename(disk)
    finally:
        if temporary.exists(): temporary.unlink()
    subprocess.run(['python3',str(root/'scripts/configure-debian.py')],check=True)
initrd = assets/'debian-initrd.img'
if not initrd.exists():
    with tempfile.TemporaryDirectory(prefix='rvvm-initrd-mount-') as directory:
        subprocess.run(['mount','-o','ro,loop,offset=134217728',str(disk),directory],check=True)
        try:
            boot = pathlib.Path(directory)/'boot'
            source = next(boot.glob('initrd.img-6.12.107+deb13-riscv64'))
            shutil.copyfile(source,initrd)
        finally: subprocess.run(['umount',directory],check=True)
print('Working disk and original Debian initrd ready.')
