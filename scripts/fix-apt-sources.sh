#!/bin/sh
# Move only NanoRV's misplaced backup. Preserve its contents and active sources.
set -eu
base=${1:-/}
source=${base%/}/etc/apt/sources.list.d/debian.sources.nanorv-original
backups=${base%/}/etc/apt/nanorv-backups
if [ -f "$source" ]; then
  mkdir -p "$backups"
  destination=$backups/debian.sources.nanorv-original
  if [ -e "$destination" ]; then
    destination=$destination.migrated.$$
  fi
  mv "$source" "$destination"
fi
