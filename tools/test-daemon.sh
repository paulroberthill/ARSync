#!/bin/sh
# Runs a throwaway rsync daemon in the foreground for testing arsync from an
# emulated or real Amiga. Ctrl-C stops it and removes its files.
#
# Usage: tools/test-daemon.sh [ADDRESS] [PORT]
#   ADDRESS defaults to 127.0.0.1, which is right for FS-UAE's built-in
#   bsdsocket emulation (the Amiga shares the host's network stack). Use
#   0.0.0.0 when the Amiga reaches this machine over a real or emulated NIC.
set -eu

ADDR=${1:-127.0.0.1}
PORT=${2:-8730}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/arsync-daemon.XXXXXX")
PID=
cleanup() {
    [ -n "$PID" ] && kill "$PID" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT
trap 'exit 130' INT TERM

mkdir -p "$WORK/test/sub"

cp /mnt/hd/LightwaveRTG.lha "$WORK/test"
ls -asl "$WORK/test"

echo "hello from the rsync daemon" > "$WORK/test/hello.txt"
head -c 100000 /dev/urandom > "$WORK/test/sub/random.bin"
cat > "$WORK/motd" <<'EOF'
arsync test daemon - message of the day
EOF
cat > "$WORK/rsyncd.conf" <<EOF
use chroot = no
motd file = $WORK/motd

[test]
    path = $WORK/test
    comment = arsync test module
    read only = yes

[private]
    path = $WORK/test
    comment = needs a password
    auth users = amiga
    secrets file = $WORK/secrets
    strict modes = no
EOF
echo "amiga:secret" > "$WORK/secrets"

echo "rsync daemon on $ADDR:$PORT (modules: test, private [user amiga, password secret])"
echo "Ctrl-C to stop."
# stdin must not be a socket, or the daemon assumes inetd mode.
rsync --daemon --no-detach --log-file=/dev/stdout \
      --config="$WORK/rsyncd.conf" --address="$ADDR" --port="$PORT" </dev/null &
PID=$!
wait "$PID"
