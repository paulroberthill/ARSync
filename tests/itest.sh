#!/bin/sh
# Integration tests: runs the host build of arsync against a private
# `rsync --daemon` and compares its output and exit codes with the real
# rsync client's for the same requests.
#
# Usage: tests/itest.sh [path/to/arsync]
set -u

ARSYNC=${1:-build/host/arsync}
RSYNC=${RSYNC:-rsync}
WORK=$(mktemp -d "${TMPDIR:-/tmp}/arsync-itest.XXXXXX")
PASS=0
FAIL=0

cleanup() {
    [ -f "$WORK/rsyncd.pid" ] && kill "$(cat "$WORK/rsyncd.pid")" 2>/dev/null
    rm -rf "$WORK"
}
trap cleanup EXIT INT TERM

# --- daemon setup -----------------------------------------------------------

mkdir -p "$WORK/pub" "$WORK/hidden"
cat > "$WORK/motd" <<'EOF'
Welcome to the arsync test daemon.
Second MOTD line.
EOF
cat > "$WORK/rsyncd.conf" <<EOF
pid file = $WORK/rsyncd.pid
log file = $WORK/rsyncd.log
use chroot = no
motd file = $WORK/motd

[pub]
    path = $WORK/pub
    comment = Public files
    read only = yes

[hidden]
    path = $WORK/hidden
    list = no

[nocomment]
    path = $WORK/pub

[private]
    path = $WORK/pub
    comment = needs a password
    auth users = amiga
    secrets file = $WORK/secrets
    strict modes = no
EOF
echo "amiga:secret" > "$WORK/secrets"
printf 'secret\n' > "$WORK/pw"
chmod 600 "$WORK/pw"
mkdir -p "$WORK/pub/sub"
echo hello > "$WORK/pub/sub/file"

# Pick a free port by letting the daemon fail on busy ones.
PORT=
for p in $(seq 28730 28790); do
    # stdin must not be a socket, or the daemon assumes inetd mode.
    if "$RSYNC" --daemon --config="$WORK/rsyncd.conf" --port="$p" --address=127.0.0.1 </dev/null 2>/dev/null; then
        i=0
        while [ ! -s "$WORK/rsyncd.pid" ] && [ $i -lt 50 ]; do sleep 0.1; i=$((i + 1)); done
        if [ -s "$WORK/rsyncd.pid" ] && kill -0 "$(cat "$WORK/rsyncd.pid")" 2>/dev/null; then
            PORT=$p
            break
        fi
    fi
done
if [ -z "$PORT" ]; then
    echo "could not start rsync daemon" >&2
    exit 1
fi
# The daemon may log the bind failure and exit; give it a moment, then check.
sleep 0.3
kill -0 "$(cat "$WORK/rsyncd.pid")" 2>/dev/null || { echo "rsync daemon died" >&2; exit 1; }

# --- helpers ----------------------------------------------------------------

# compare NAME ARGS... : run both clients with ARGS, compare stdout and exit code.
compare() {
    name=$1
    shift
    "$RSYNC" "$@" > "$WORK/want.out" 2> "$WORK/want.err"
    want=$?
    "$ARSYNC" "$@" > "$WORK/got.out" 2> "$WORK/got.err"
    got=$?
    if [ "$want" = "$got" ] && cmp -s "$WORK/want.out" "$WORK/got.out"; then
        PASS=$((PASS + 1))
        echo "ok   $name"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name (exit: rsync $want, arsync $got)"
        diff "$WORK/want.out" "$WORK/got.out" | sed 's/^/     /'
        sed 's/^/     arsync stderr: /' "$WORK/got.err"
    fi
}

# expect NAME CODE ARGS... : arsync alone must exit with CODE.
expect() {
    name=$1
    code=$2
    shift 2
    "$ARSYNC" "$@" > "$WORK/got.out" 2> "$WORK/got.err"
    got=$?
    if [ "$got" = "$code" ]; then
        PASS=$((PASS + 1))
        echo "ok   $name"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name (exit $got, wanted $code)"
        sed 's/^/     arsync stderr: /' "$WORK/got.err"
    fi
}

# --- tests ------------------------------------------------------------------

compare "module list (rsync://)" "rsync://127.0.0.1:$PORT/"
compare "module list (no slash)" "rsync://127.0.0.1:$PORT"
compare "module list (host::)" --port="$PORT" "127.0.0.1::"
compare "module list --no-motd" --no-motd "rsync://127.0.0.1:$PORT/"
compare "module list as user" "rsync://someone@localhost:$PORT/"

# A closed port: rsync reports a socket error (10).
CLOSED=$((PORT + 100))
compare "connection refused" "rsync://127.0.0.1:$CLOSED/"
expect "unknown host" 10 "rsync://no-such-host.invalid/"
expect "local path rejected" 1 "Work:dir"
expect "bad option" 1 --bogus "rsync://127.0.0.1:$PORT/"

# --- authentication and protocol setup (M2) ---------------------------------
# Until the file list is implemented (M3), a successful handshake ends with
# exit code 4 ("not implemented yet") once the server starts sending it.

URL="rsync://127.0.0.1:$PORT"
AUTHURL="rsync://amiga@127.0.0.1:$PORT/private/"
export RSYNC_PASSWORD=wrong
compare "auth failure" "$AUTHURL" "$WORK/dest"
unset RSYNC_PASSWORD
compare "unknown module" "$URL/nosuchmodule/" "$WORK/dest"
expect "handshake, public module" 4 "$URL/pub/" "$WORK/dest"
expect "handshake, module subpath" 4 "$URL/pub/sub" "$WORK/dest"
export RSYNC_PASSWORD=secret
expect "auth via RSYNC_PASSWORD" 4 "$AUTHURL" "$WORK/dest"
unset RSYNC_PASSWORD
expect "auth via --password-file" 4 --password-file="$WORK/pw" "$AUTHURL" "$WORK/dest"
expect "missing password file" 1 --password-file="$WORK/nope" "$AUTHURL" "$WORK/dest"

"$ARSYNC" -vv --no-motd "$URL/pub/" "$WORK/dest" > "$WORK/got.out" 2>&1
if grep -q "checksum md5" "$WORK/got.out" && grep -q "handshake complete" "$WORK/got.out"; then
    PASS=$((PASS + 1))
    echo "ok   checksum negotiated as md5"
else
    FAIL=$((FAIL + 1))
    echo "FAIL checksum negotiated as md5"
    sed 's/^/     /' "$WORK/got.out"
fi

echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
