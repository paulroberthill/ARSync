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

[tree]
    path = $WORK/tree
    comment = listing and sort-order fixtures

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

# A tree that exercises rsync's sort order and the listing format: names
# with bytes below and above '/', files and directories with similar
# names, sizes that need grouping (one past 4 GiB, sparse), times before
# the Amiga epoch and after 2038, nanosecond times, and odd mode bits.
T="$WORK/tree"
mkdir -p "$T/b" "$T/b-y" "$T/b.d" "$T/d1/d2/d3" "$T/empty" "$T/.git" "$T/foo" "$T/sticky"
for f in a B b.txt "b+" "b y" z "~tilde" _u .hidden foo.bar foo-bar b/x b-y/y d1/d2/d3/deep \
         "$(printf 'caf\303\251')" b.d/in a.e; do
    printf 'x' > "$T/$f"
done
head -c 999 /dev/zero > "$T/size999"
head -c 1000 /dev/zero > "$T/size1000"
head -c 1234567 /dev/zero > "$T/size1234567"
: > "$T/zero"
truncate -s 5000000000 "$T/big-sparse"
touch -d "1975-06-15 12:00:00" "$T/old-1975"
touch -d "2040-02-29 23:59:59" "$T/future-2040"
touch -d "2026-01-02 03:04:05.123456789" "$T/nsec"
chmod 4755 "$T/a"
chmod 2750 "$T/B"
chmod 1777 "$T/sticky"
chmod 0444 "$T/z"
chmod 0600 "$T/_u"
ln -s a "$T/link-to-a"

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
# Listing (no destination) exercises the whole handshake.

URL="rsync://127.0.0.1:$PORT"
AUTHURL="rsync://amiga@127.0.0.1:$PORT/private/"
export RSYNC_PASSWORD=wrong
compare "auth failure" "$AUTHURL"
export RSYNC_PASSWORD=secret
compare "auth via RSYNC_PASSWORD" "$AUTHURL"
unset RSYNC_PASSWORD
compare "unknown module" "$URL/nosuchmodule/"
expect "auth via --password-file" 0 --password-file="$WORK/pw" "$AUTHURL"
expect "missing password file" 1 --password-file="$WORK/nope" "$AUTHURL"

"$ARSYNC" -vv --no-motd "$URL/pub/" > "$WORK/got.out" 2>&1
if grep -q "checksum md5" "$WORK/got.out"; then
    PASS=$((PASS + 1))
    echo "ok   checksum negotiated as md5"
else
    FAIL=$((FAIL + 1))
    echo "FAIL checksum negotiated as md5"
    sed 's/^/     /' "$WORK/got.out"
fi

# --- file listing (M3) -----------------------------------------------------

TREE="$URL/tree"
compare "list module top" "$TREE/"
compare "list -r" -r "$TREE/"
compare "list -r --list-only" -r --list-only "$TREE/"
compare "list subdir contents" "$TREE/b/"
compare "list subdir itself" "$TREE/b"
compare "list nested -r" -r "$TREE/d1"
compare "list single file" "$TREE/size1234567"
compare "list empty dir" "$TREE/empty/"
compare "list missing path" "$TREE/nope"
compare "list as --list-only with dest" --list-only "$TREE/" "$WORK/dest"
for tz in UTC Asia/Kolkata America/St_Johns Pacific/Kiritimati; do
    TZ=$tz compare "list -r in TZ=$tz" -r "$TREE/"
done

# -v adds a header and a summary; the rate depends on timing, so drop it.
# rsync's own client would use incremental recursion with -r (we never
# offer it), which changes the byte counts, so compare against it with
# that turned off.
"$RSYNC" --no-inc-recursive -v -r "$TREE/" 2>&1 | sed 's/  [0-9.,]* bytes\/sec$//' > "$WORK/want.out"
"$ARSYNC" -v -r "$TREE/" 2>&1 | sed 's/  [0-9.,]* bytes\/sec$//' > "$WORK/got.out"
if cmp -s "$WORK/want.out" "$WORK/got.out"; then
    PASS=$((PASS + 1))
    echo "ok   list -v -r (summary byte counts)"
else
    FAIL=$((FAIL + 1))
    echo "FAIL list -v -r (summary byte counts)"
    diff "$WORK/want.out" "$WORK/got.out" | sed 's/^/     /'
fi

echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
