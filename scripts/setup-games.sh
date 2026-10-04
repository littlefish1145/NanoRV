#!/bin/sh
# Run as root inside Debian. Keep TLS and APT signature checks enabled.
set -eu
export DEBIAN_FRONTEND=noninteractive
# Keep binary repository caches between commands on this emulated CPU.
if [ ! -e /etc/apt/apt.conf.d/99nanorv-cache ]; then
  printf 'Dir::Cache::pkgcache "pkgcache.bin";\nDir::Cache::srcpkgcache "srcpkgcache.bin";\n' > /etc/apt/apt.conf.d/99nanorv-cache
fi
if [ "${1:-}" = '--configure-only' ]; then
  test -x /usr/games/neverball && command -v Xorg >/dev/null
else
  apt-get update
  apt-get install -y --no-install-recommends xserver-xorg-core xserver-xorg-input-libinput xserver-xorg-video-fbdev xinit x11-xserver-utils mesa-utils mesa-vulkan-drivers vulkan-tools libgl1-mesa-dri libegl-mesa0 neverball
fi
mkdir -p /usr/local/bin
cat > /usr/local/bin/nanorv-games <<'EOF'
#!/bin/sh
set -eu
export XDG_RUNTIME_DIR=/run/user/0
export PATH=/usr/games:/usr/local/bin:/usr/bin:/bin:$PATH
export SDL_AUDIODRIVER=${SDL_AUDIODRIVER:-dummy}
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
game=${1:-neverball}
if ! command -v "$game" >/dev/null; then
  echo "Game not installed: $game" >&2; exit 1
fi
# modesetting uses the virtio-gpu DRM driver and VirGL. Leave Mesa driver choice
# automatic so an unsupported host falls back according to Mesa's capabilities.
exec xinit "$(command -v "$game")" -- :0 -nolisten tcp -noreset
EOF
chmod 755 /usr/local/bin/nanorv-games
cat > /etc/systemd/system/nanorv-games.service <<'EOF'
[Unit]
Description=NanoRV OpenGL game
ConditionKernelCommandLine=nanorv.games=1
After=systemd-modules-load.service systemd-user-sessions.service

[Service]
Type=simple
ExecStart=/usr/local/bin/nanorv-games
Restart=no

[Install]
WantedBy=multi-user.target
EOF
systemctl enable nanorv-games.service
echo 'Installed. run-games.ps1 starts Neverball automatically.'
echo 'To launch another installed game from the Debian console: nanorv-games GAME'
echo 'For renderer information: xinit /usr/bin/glxinfo -B -- :0 -nolisten tcp'
echo 'Vulkan currently uses Mesa lavapipe software rendering.'
