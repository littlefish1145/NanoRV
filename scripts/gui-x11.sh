#!/bin/sh
# Start the guest X server on the NanoRV VirtIO GPU. The VM window shows the
# picture, so no host X server is required; the serial console stays usable.
set -eu
if [ -e /tmp/.X11-unix/X0 ]; then
  echo 'X server already running on :0.'
  exit 0
fi
if [ ! -e /dev/dri/card0 ]; then
  echo 'No /dev/dri/card0. Boot with graphics enabled (run-debian.ps1 default).' >&2
  exit 1
fi
mkdir -p /tmp/.X11-unix
chmod 1777 /tmp/.X11-unix
export XDG_RUNTIME_DIR=${XDG_RUNTIME_DIR:-/run/user/0}
mkdir -p "$XDG_RUNTIME_DIR"
chmod 700 "$XDG_RUNTIME_DIR"
# -keeptty keeps the serial console, -novtswitch avoids racing the serial getty.
exec Xorg :0 -keeptty -novtswitch -nolisten tcp -noreset "$@"
