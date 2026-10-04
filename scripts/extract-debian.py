"""One-time, offline image preparation under WSL; never replaces a working disk.

Extracts the guest disk, kernel and initramfs from the official Debian nocloud
image. Existing files are left alone so users, packages and passwords survive.
"""
import gzip
import pathlib
import shutil
import tarfile
import tempfile
import subprocess

root = pathlib.Path(__file__).resolve().parent.parent
assets = root / 'assets'
disk = assets / 'disk.raw'
kernel = assets / 'linux'
initrd = assets / 'debian-boot-initrd.cpio'
OFFSET = 134217728  # partition 1 starts 128 MiB into the image


def extract_disk():
    temporary = assets / 'disk.raw.preparing'
    try:
        with tarfile.open(assets / 'debian-13-nocloud-riscv64.tar.xz', 'r:xz') as archive:
            member = next(m for m in archive if pathlib.PurePosixPath(m.name).name == 'disk.raw' and m.isfile())
            with archive.extractfile(member) as source, temporary.open('wb') as output:
                shutil.copyfileobj(source, output, 1024 * 1024)
        temporary.rename(disk)
    finally:
        if temporary.exists():
            temporary.unlink()


def newest(directory, pattern):
    return sorted(directory.glob(pattern), key=lambda p: p.name)[-1]


def extract_boot_files():
    """Copy the kernel and initramfs out of the guest image's /boot."""
    if kernel.exists() and initrd.exists():
        return
    with tempfile.TemporaryDirectory(prefix='rvvm-boot-mount-') as directory:
        subprocess.run(['mount', '-o', f'ro,loop,offset={OFFSET}', str(disk), directory], check=True)
        try:
            boot = pathlib.Path(directory) / 'boot'
            if not kernel.exists():
                # Debian ships the raw RV64 Image as "vmlinux" on this arch.
                image = newest(boot, 'vmlinux-*')
                shutil.copyfile(image, kernel)
                print('kernel:', image.name, '->', kernel.name)
            if not initrd.exists():
                archive = newest(boot, 'initrd.img-*')
                shutil.copyfile(archive, initrd)
                print('initrd:', archive.name, '->', initrd.name, f'({initrd.stat().st_size} bytes)')
        finally:
            subprocess.run(['umount', directory], check=True)


if not disk.exists():
    extract_disk()
    subprocess.run(['python3', str(root / 'scripts/configure-debian.py')], check=True)
extract_boot_files()
print('Working disk, kernel and initramfs ready.')
