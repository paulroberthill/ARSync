#!/bin/sh
# Installs vamos (amitools), which runs AmigaOS CLI binaries on Linux by
# emulating a 68k CPU and parts of exec/dos.library. Used by `make test-amiga`
# to run the unit tests as real big-endian 68k code. It has no TCP/IP stack,
# so anything network-related still needs FS-UAE or real hardware.
#
# Versions are pinned: amitools 0.8.1 fails with machine68k 0.4.x
# ("'machine68k.Traps' object has no attribute 'set_exc_func'").
set -e
cd "$(dirname "$0")/.."
python3 -m venv .venv
.venv/bin/pip install --quiet "amitools==0.8.1" "machine68k==0.3.0"
.venv/bin/vamos --help >/dev/null
echo "vamos installed in .venv/bin/vamos"
