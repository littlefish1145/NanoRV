#!/bin/sh
# Refresh signed repository indexes without rebuilding binary caches immediately.
# Ordinary apt install/search commands still validate and regenerate their caches.
set -eu
exec apt-get -o pkgCacheFile::Generate=false --error-on=any update "$@"
