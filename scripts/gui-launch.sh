#!/bin/sh
# Start GUI programs with a usable session bus, including after su/sudo.
set -eu
if [ "$#" -eq 0 ]; then
  echo 'Usage: rvvm-gui PROGRAM [ARGUMENTS...]' >&2
  exit 2
fi
if [ -z "${DISPLAY:-}" ] && [ -r /etc/profile.d/rvvm-x11.sh ]; then
  . /etc/profile.d/rvvm-x11.sh
fi
if [ -z "${DISPLAY:-}" ] && [ -r /etc/rvvm-desktop/session.json ] && [ -r /run/rvvm-desktop/Xauthority ]; then
  DISPLAY=$(python3 -c 'import json; n=int(json.load(open("/etc/rvvm-desktop/session.json"))["display_number"]); assert 1<=n<=200; print("10.0.2.2:"+str(n))')
  export DISPLAY XAUTHORITY=/run/rvvm-desktop/Xauthority
fi
if [ -z "${DISPLAY:-}" ]; then
  echo 'No X11 display. Start Debian with run-debian.ps1 without -NoX11, or use run-app.ps1.' >&2
  exit 1
fi
if [ -n "${DBUS_SESSION_BUS_ADDRESS:-}" ] && command -v dbus-send >/dev/null 2>&1 &&
    dbus-send --session --print-reply --reply-timeout=1000 --dest=org.freedesktop.DBus /org/freedesktop/DBus org.freedesktop.DBus.GetId >/dev/null 2>&1; then
  exec "$@"
fi
unset DBUS_SESSION_BUS_ADDRESS DBUS_SESSION_BUS_PID DBUS_SESSION_BUS_WINDOWID
if ! command -v dbus-run-session >/dev/null 2>&1; then
  echo 'dbus-run-session is missing; install the Debian dbus-daemon package.' >&2
  exit 127
fi
exec dbus-run-session -- "$@"
