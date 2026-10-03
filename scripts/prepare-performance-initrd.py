"""Run in WSL. Add the local game and reversible apt tuning to the fast initrd."""
import pathlib,subprocess,tempfile,shutil
root=pathlib.Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='rvvm-performance-initrd-') as directory:
    work=pathlib.Path(directory)
    subprocess.run(['cpio','-id','--no-absolute-filenames'],cwd=work,input=(root/'assets/debian-initrd.cpio').read_bytes(),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    shutil.copy2(root/'build/rvvm-breakout',work/'bin/rvvm-breakout')
    (work/'bin/rvvm-breakout').chmod(0o755)
    shutil.copy2(root/'scripts/setup-games.sh',work/'bin/rvvm-setup-games')
    (work/'bin/rvvm-setup-games').chmod(0o755)
    shutil.copy2(root/'scripts/apt-update.sh',work/'bin/rvvm-apt-update')
    (work/'bin/rvvm-apt-update').chmod(0o755)
    shutil.copy2(root/'scripts/fix-apt-sources.sh',work/'bin/rvvm-apt-fix')
    (work/'bin/rvvm-apt-fix').chmod(0o755)
    init=work/'init'
    config='''
# Install the bundled Linux framebuffer game; preserve local changes on later boots.
/bin/busybox mkdir -p /newroot/usr/local/bin /newroot/etc/apt/apt.conf.d
/bin/busybox cp /bin/rvvm-breakout /newroot/usr/local/bin/rvvm-breakout
/bin/busybox chmod 755 /newroot/usr/local/bin/rvvm-breakout
/bin/busybox cp /bin/rvvm-setup-games /newroot/usr/local/bin/rvvm-setup-games
/bin/busybox chmod 755 /newroot/usr/local/bin/rvvm-setup-games
/bin/busybox cp /bin/rvvm-apt-update /newroot/usr/local/bin/rvvm-apt-update
/bin/busybox chmod 755 /newroot/usr/local/bin/rvvm-apt-update
/bin/busybox cp /bin/rvvm-apt-fix /newroot/usr/local/bin/rvvm-apt-fix
/bin/busybox chmod 755 /newroot/usr/local/bin/rvvm-apt-fix
# Preserve the old misplaced backup, including on already-configured disks.
backups=/newroot/etc/apt/rvvm-backups
/bin/busybox mkdir -p "$backups"
legacy=/newroot/etc/apt/sources.list.d/debian.sources.rvvm-original
if [ -f "$legacy" ]; then
  destination=$backups/debian.sources.rvvm-original
  [ ! -e "$destination" ] || destination=$destination.migrated.$$
  /bin/busybox mv "$legacy" "$destination"
fi
# This NAT currently supports IPv4. Leave signature/TLS verification enabled.
network=/newroot/etc/apt/apt.conf.d/99rvvm-network
if [ ! -e "$network" ] || /bin/busybox grep -q '^// RVVM network defaults' "$network" || [ "$(cat "$network")" = 'Acquire::ForceIPv4 "true"; Acquire::Retries "3";' ]; then
  echo '// RVVM network defaults
Acquire::ForceIPv4 "true";
Acquire::Retries "3";
Acquire::Languages "none";
Acquire::CompressionTypes::Order { "gz"; "xz"; };
Dir::Cache::pkgcache "pkgcache.bin";
Dir::Cache::srcpkgcache "srcpkgcache.bin";' > "$network"
fi
# Avoid apt 3.0.3 mirror-method retry stalls. Keep suites, components and deb-src.
source=/newroot/etc/apt/sources.list.d/debian.sources
if [ -f "$source" ] && [ ! -e /newroot/etc/apt/rvvm-mirror-configured ]; then
  [ -e "$backups/debian.sources.rvvm-original" ] || /bin/busybox cp "$source" "$backups/debian.sources.rvvm-original"
  /bin/busybox sed -i 's|mirror+file:///etc/apt/mirrors/debian.list|https://mirrors.tuna.tsinghua.edu.cn/debian|g; s|mirror+file:///etc/apt/mirrors/debian-security.list|https://mirrors.tuna.tsinghua.edu.cn/debian-security|g; s|https://deb.debian.org/debian-security|https://mirrors.tuna.tsinghua.edu.cn/debian-security|g; s|https://deb.debian.org/debian|https://mirrors.tuna.tsinghua.edu.cn/debian|g' "$source"
  echo configured > /newroot/etc/apt/rvvm-mirror-configured
fi
for argument in $(cat /proc/cmdline); do
  case "$argument" in
    rvvm.graphics=1)
      /bin/busybox mkdir -p /newroot/etc/modules-load.d
      echo 'virtio_gpu
virtio_input
evdev' > /newroot/etc/modules-load.d/rvvm-display.conf
      ;;
    rvvm.http_proxy=*)
      proxy=${argument#rvvm.http_proxy=}
      echo "Acquire::http::Proxy \\\"$proxy\\\"; Acquire::https::Proxy \\\"$proxy\\\";" > /newroot/etc/apt/apt.conf.d/98rvvm-proxy
      ;;
  esac
done
'''
    text=init.read_text().replace('mount -o move /dev /newroot/dev',config+'\nmount -o move /dev /newroot/dev')
    text=text.replace('exec switch_root /newroot /sbin/init','''if /bin/busybox grep -q 'rvvm.shell=1' /newroot/proc/cmdline; then
  export PS1='RVVM_SHELL# '
  export PATH=/usr/local/sbin:/usr/local/bin:/usr/sbin:/usr/bin:/sbin:/bin
  exec switch_root /newroot /bin/bash --noprofile --norc -i
fi
exec switch_root /newroot /sbin/init''')
    init.write_text(text)
    paths=subprocess.check_output(['find','.','-print0'],cwd=work)
    destination=root/'assets/debian-performance-initrd.cpio'
    with destination.open('wb') as file:
        subprocess.run(['cpio','--null','-o','-H','newc','--owner=0:0'],cwd=work,input=paths,stdout=file,check=True)
print('Performance initrd:',destination)
