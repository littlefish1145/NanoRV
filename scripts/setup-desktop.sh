#!/bin/sh
set -eu
test "$(id -u)" = 0 || { echo 'Run rvvm-setup-desktop as root.' >&2; exit 1; }
if [ "${1:-}" != '--configure-only' ]; then
  export DEBIAN_FRONTEND=noninteractive
  rvvm-apt-fix
  apt-get --error-on=any update
  apt-get install -y --no-install-recommends python3 xauth xterm x11-apps xclip dbus-x11 mousepad pcmanfm fonts-dejavu-core fonts-noto-cjk fastfetch
fi
command -v python3 >/dev/null
command -v xterm >/dev/null
command -v dbus-run-session >/dev/null
id rvvm >/dev/null 2>&1 || useradd --create-home --shell /bin/bash rvvm
cat > /etc/systemd/system/rvvm-desktop.service <<'EOF'
[Unit]
Description=RVVM independent desktop applications
ConditionKernelCommandLine=rvvm.desktop=1
After=network-online.target
Wants=network-online.target

[Service]
Type=simple
ExecStart=/usr/bin/python3 /usr/local/bin/rvvm-desktop-agent /etc/rvvm-desktop/session.json
Restart=on-failure
RestartSec=5
UMask=0077

[Install]
WantedBy=multi-user.target
EOF
systemctl enable rvvm-desktop.service
echo 'Desktop apps installed. On Windows, run .\run-app.ps1 -App xterm'
