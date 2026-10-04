"""Run in WSL. Add the local game and reversible apt tuning to the fast initrd."""
import pathlib,subprocess,tempfile,shutil
root=pathlib.Path(__file__).resolve().parent.parent
with tempfile.TemporaryDirectory(prefix='nanorv-performance-initrd-') as directory:
    work=pathlib.Path(directory)
    subprocess.run(['cpio','-id','--no-absolute-filenames'],cwd=work,input=(root/'assets/debian-initrd.cpio').read_bytes(),stdout=subprocess.DEVNULL,stderr=subprocess.DEVNULL,check=True)
    shutil.copy2(root/'build/nanorv-breakout',work/'bin/nanorv-breakout')
    (work/'bin/nanorv-breakout').chmod(0o755)
    shutil.copy2(root/'scripts/setup-games.sh',work/'bin/nanorv-setup-games')
    (work/'bin/nanorv-setup-games').chmod(0o755)
    shutil.copy2(root/'scripts/apt-update.sh',work/'bin/nanorv-apt-update')
    (work/'bin/nanorv-apt-update').chmod(0o755)
    shutil.copy2(root/'scripts/fix-apt-sources.sh',work/'bin/nanorv-apt-fix')
    (work/'bin/nanorv-apt-fix').chmod(0o755)
    init=work/'init'
    config='''
# Install the bundled Linux framebuffer game; preserve local changes on later boots.
/bin/busybox mkdir -p /newroot/usr/local/bin /newroot/etc/apt/apt.conf.d
/bin/busybox cp /bin/nanorv-breakout /newroot/usr/local/bin/nanorv-breakout
/bin/busybox chmod 755 /newroot/usr/local/bin/nanorv-breakout
/bin/busybox cp /bin/nanorv-setup-games /newroot/usr/local/bin/nanorv-setup-games
/bin/busybox chmod 755 /newroot/usr/local/bin/nanorv-setup-games
/bin/busybox cp /bin/nanorv-apt-update /newroot/usr/local/bin/nanorv-apt-update
/bin/busybox chmod 755 /newroot/usr/local/bin/nanorv-apt-update
/bin/busybox cp /bin/nanorv-apt-fix /newroot/usr/local/bin/nanorv-apt-fix
/bin/busybox chmod 755 /newroot/usr/local/bin/nanorv-apt-fix
# Preserve the old misplaced backup, including on already-configured disks.
backups=/newroot/etc/apt/nanorv-backups
/bin/busybox mkdir -p "$backups"
legacy=/newroot/etc/apt/sources.list.d/debian.sources.nanorv-original
if [ -f "$legacy" ]; then
  destination=$backups/debian.sources.nanorv-original
  [ ! -e "$destination" ] || destination=$destination.migrated.$$
  /bin/busybox mv "$legacy" "$destination"
fi
# This NAT currently supports IPv4. Leave signature/TLS verification enabled.
network=/newroot/etc/apt/apt.conf.d/99nanorv-network
if [ ! -e "$network" ] || /bin/busybox grep -q '^// NanoRV network defaults' "$network" || [ "$(cat "$network")" = 'Acquire::ForceIPv4 "true"; Acquire::Retries "3";' ]; then
  echo '// NanoRV network defaults
Acquire::ForceIPv4 "true";
Acquire::Retries "3";
Acquire::Languages "none";
Acquire::CompressionTypes::Order { "gz"; "xz"; };
Dir::Cache::pkgcache "pkgcache.bin";
Dir::Cache::srcpkgcache "srcpkgcache.bin";' > "$network"
fi
# Avoid apt 3.0.3 mirror-method retry stalls. Keep suites, components and deb-src.
source=/newroot/etc/apt/sources.list.d/debian.sources
if [ -f "$source" ] && [ ! -e /newroot/etc/apt/nanorv-mirror-configured ]; then
  [ -e "$backups/debian.sources.nanorv-original" ] || /bin/busybox cp "$source" "$backups/debian.sources.nanorv-original"
  /bin/busybox sed -i 's|mirror+file:///etc/apt/mirrors/debian.list|https://mirrors.tuna.tsinghua.edu.cn/debian|g; s|mirror+file:///etc/apt/mirrors/debian-security.list|https://mirrors.tuna.tsinghua.edu.cn/debian-security|g; s|https://deb.debian.org/debian-security|https://mirrors.tuna.tsinghua.edu.cn/debian-security|g; s|https://deb.debian.org/debian|https://mirrors.tuna.tsinghua.edu.cn/debian|g' "$source"
  echo configured > /newroot/etc/apt/nanorv-mirror-configured
fi
for argument in $(cat /proc/cmdline); do
  case "$argument" in
    nanorv.graphics=1)
      /bin/busybox mkdir -p /newroot/etc/modules-load.d
      echo 'virtio_gpu
virtio_input
evdev' > /newroot/etc/modules-load.d/nanorv-display.conf
      ;;
    nanorv.http_proxy=*)
      proxy=${argument#nanorv.http_proxy=}
      echo "Acquire::http::Proxy \\\"$proxy\\\"; Acquire::https::Proxy \\\"$proxy\\\";" > /newroot/etc/apt/apt.conf.d/98nanorv-proxy
      ;;
  esac
done
'''
    text=init.read_text().replace('mount -o move /dev /newroot/dev',config+'\nmount -o move /dev /newroot/dev')
    text=text.replace('exec switch_root /newroot /sbin/init','''if /bin/busybox grep -q 'nanorv.shell=1' /newroot/proc/cmdline; then
  export PS1='NanoRV_SHELL# '
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
