#!/bin/sh
# Move only RVVM's misplaced backup. Preserve its contents and active sources.
set -eu
base=${1:-/}
source=${base%/}/etc/apt/sources.list.d/debian.sources.rvvm-original
backups=${base%/}/etc/apt/rvvm-backups
if [ -f "$source" ]; then
  mkdir -p "$backups"
  destination=$backups/debian.sources.rvvm-original
  if [ -e "$destination" ]; then
    destination=$destination.migrated.$$
  fi
  mv "$source" "$destination"
fi
