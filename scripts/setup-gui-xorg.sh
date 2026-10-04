#!/bin/sh
# Run as root inside Debian. One-time install of the guest X11 server so GUI
# programs render on the NanoRV VirtIO GPU instead of a host X server.
set -eu
export DEBIAN_FRONTEND=noninteractive
marker=/var/lib/nanorv-xorg-ready
if [ -e "$marker" ]; then
  exit 0
fi
if command -v Xorg >/dev/null 2>&1; then
  mkdir -p /var/lib && : >"$marker"
  exit 0
fi
# Keep binary repository caches between commands on this emulated CPU.
if [ ! -e /etc/apt/apt.conf.d/99nanorv-cache ]; then
  printf 'Dir::Cache::pkgcache "pkgcache.bin";\nDir::Cache::srcpkgcache "srcpkgcache.bin";\n' > /etc/apt/apt.conf.d/99nanorv-cache
fi
apt-get update
# The modesetting driver in xserver-xorg-core drives the virtio-gpu DRM device,
# so Mesa's VirGL driver accelerates GL inside the VM window.
apt-get install -y --no-install-recommends \
  xserver-xorg-core xserver-xorg-input-libinput xserver-xorg-video-fbdev \
  x11-xserver-utils xinit mesa-utils
mkdir -p /var/lib && : >"$marker"
echo 'Installed. The NanoRV X server starts on the next boot; use nanorv-gui PROGRAM.'
