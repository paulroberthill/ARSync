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

[xfer]
    path = $WORK/xfer
    comment = transfer fixtures

[many]
    path = $WORK/many

[names]
    path = $WORK/names

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

# Transfer fixtures: like the tree above minus the 5 GB file, plus data
# that spans many 32 KiB chunks, and a file the daemon can't read.
X="$WORK/xfer"
mkdir -p "$X"
cp -a "$T/." "$X/"
rm "$X/big-sparse"
head -c 3000000 /dev/urandom > "$X/random-3m"
head -c 32768 /dev/urandom > "$X/exactly-one-chunk"
mkdir -p "$X/deep/a/b/c/d"
echo deep > "$X/deep/a/b/c/d/file"
printf 'secret' > "$X/unreadable"
chmod 000 "$X/unreadable"

# Names and sizes AmigaDOS can't hold (checked with --amiga-rules).
N="$WORK/names"
mkdir -p "$N/Docs" "$N/docs"
for f in "with:colon" "a-name-that-is-forty-characters-long.txt" Readme README Docs/x docs/y normal \
         "exactly-thirty-characters-long"; do
    echo "$f" > "$N/$f"
done
truncate -s 3000000000 "$N/big-3g"

# Lots of small files, to keep many requests in flight.
mkdir -p "$WORK/many"
i=0
while [ $i -lt 30 ]; do
    mkdir -p "$WORK/many/d$i"
    j=0
    while [ $j -lt 100 ]; do
        printf '%s-%s\n' "$i" "$j" > "$WORK/many/d$i/f$j"
        j=$((j + 1))
    done
    i=$((i + 1))
done

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

# --- transfers (M4) ----------------------------------------------------------
# Each case pulls the same thing with rsync (into $WORK/want) and arsync
# (into $WORK/got) and compares the resulting trees - names, types, sizes,
# permissions, modification times (with -t) and contents - plus stdout and
# exit codes. rsync runs with --no-inc-recursive, as arsync always does.

# Lists a tree's metadata; with "notimes", file times are left out.
tree_sig() {
    if [ "${2:-}" = notimes ]; then
        (cd "$1" && find . -printf '%P|%y|%s|%m\n' | sort)
    else
        (cd "$1" && find . ! -name . -printf '%P|%y|%s|%m|%T@\n' | sort)
    fi
}

# pull NAME SIG-MODE SETUP ARGS... : SETUP is a shell snippet run in both
# destinations first (with $D set), e.g. to pre-populate them. Use "@" in
# ARGS for the destination.
pull() {
    name=$1
    mode=$2
    setup=$3
    shift 3
    for side in want got; do
        D="$WORK/$side"
        rm -rf "$D"
        mkdir -p "$D"
        [ -n "$setup" ] && (cd "$D" && eval "$setup")
        set -- "$@"
        args=
        for a in "$@"; do
            [ "$a" = "@" ] && a="$D/dest"
            args="$args '$a'"
        done
        if [ $side = want ]; then
            eval "\"$RSYNC\" --no-inc-recursive $args" > "$WORK/$side.out" 2> "$WORK/$side.err"
        else
            eval "\"$ARSYNC\" $args" > "$WORK/$side.out" 2> "$WORK/$side.err"
        fi
        echo $? > "$WORK/$side.rc"
        # Byte counts and rates depend on timing (how the server's output
        # happens to be split into frames); destination paths differ by side.
        sed -i -e 's/^sent [0-9,]* bytes  received [0-9,]* bytes  [0-9.,]* bytes\/sec$/sent N bytes  received N bytes/' \
               -e 's/speedup is [0-9.,]*/speedup is N/' -e "s|$WORK/$side|DEST|g" "$WORK/$side.out"
    done
    ok=1
    cmp -s "$WORK/want.rc" "$WORK/got.rc" || ok=0
    cmp -s "$WORK/want.out" "$WORK/got.out" || ok=0
    if [ -e "$WORK/want/dest" ] || [ -e "$WORK/got/dest" ]; then
        tree_sig "$WORK/want/dest" "$mode" > "$WORK/want.sig" 2>/dev/null
        tree_sig "$WORK/got/dest" "$mode" > "$WORK/got.sig" 2>/dev/null
        cmp -s "$WORK/want.sig" "$WORK/got.sig" || ok=0
        diff -r "$WORK/want/dest" "$WORK/got/dest" > /dev/null 2>&1 || ok=0
    fi
    if [ $ok = 1 ]; then
        PASS=$((PASS + 1))
        echo "ok   $name"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name (exit: rsync $(cat "$WORK/want.rc"), arsync $(cat "$WORK/got.rc"))"
        diff "$WORK/want.out" "$WORK/got.out" | sed 's/^/     stdout: /'
        diff "$WORK/want.sig" "$WORK/got.sig" 2>/dev/null | head -20 | sed 's/^/     tree: /'
        sed 's/^/     arsync stderr: /' "$WORK/got.err" | head -10
    fi
}

XF="$URL/xfer"
pull "pull -rt into new dir" times "" -rt "$XF/" @
pull "pull -rtv into new dir" times "" -rtv "$XF/" @
pull "pull -rt again (nothing changes)" times \
    "'$RSYNC' -rt '$XF/' dest >/dev/null 2>&1" -rtv "$XF/" @
pull "pull -rt after local changes" times \
    "'$RSYNC' -rt '$XF/' dest >/dev/null 2>&1; echo changed > dest/a; touch -d 2001-01-01 dest/a dest/z; rm dest/b/x; rm -rf dest/d1" \
    -rtv "$XF/" @
pull "pull -r without -t" notimes "" -rv "$XF/" @
pull "pull dir without trailing slash" times "" -rtv "$XF/deep" @
pull "pull single file to new name" times "" -tv "$XF/random-3m" @
pull "pull single file into existing dir" times "mkdir dest" -tv "$XF/random-3m" @
pull "pull single file over existing file" times "echo old > dest" -t "$XF/random-3m" @
pull "pull to dest with trailing slash" times "" -t "$XF/random-3m" @/
pull "dry run -n" times "" -rtnv "$XF/" @
pull "dry run -n after copy" times \
    "'$RSYNC' -rt '$XF/' dest >/dev/null 2>&1; echo changed > dest/a; touch -d 2001-01-01 dest/a" -rtnv "$XF/" @
pull "file in place of dir and dir in place of file" times \
    "mkdir -p dest/z && : > dest/b" -rt "$XF/" @
pull "non-empty dir in place of file" times \
    "mkdir -p dest/z/keep && touch -d 2001-01-01 dest/z/keep dest/z" -rt "$XF/" @
pull "3000 small files" times "" -rt "$URL/many/" @
pull "3000 small files again" times "'$RSYNC' -rt '$URL/many/' dest >/dev/null 2>&1" -rtv "$URL/many/" @

# --- delta transfers (M5) ----------------------------------------------------
# Start from a complete copy, damage it in different ways (and backdate it so
# the quick check can't skip it), then pull again.

COPY="'$RSYNC' -rt '$XF/' dest >/dev/null 2>&1"
OLD="touch -d 2001-01-01"
pull "delta: bytes changed mid-file" times \
    "$COPY; printf XXXXXXXX | dd of=dest/random-3m bs=1 seek=1500000 conv=notrunc 2>/dev/null; $OLD dest/random-3m" \
    -rtv "$XF/" @
pull "delta: local copy longer" times \
    "$COPY; head -c 50000 /dev/urandom >> dest/random-3m; $OLD dest/random-3m" -rtv "$XF/" @
pull "delta: local copy shorter" times \
    "$COPY; truncate -s 2000000 dest/random-3m; $OLD dest/random-3m" -rtv "$XF/" @
pull "delta: bytes inserted at the start" times \
    "$COPY; { printf 'inserted'; cat dest/random-3m; } > dest/x && mv dest/x dest/random-3m; $OLD dest/random-3m" \
    -rtv "$XF/" @
pull "delta: completely different, same size" times \
    "$COPY; head -c 3000000 /dev/urandom > dest/random-3m; $OLD dest/random-3m" -rtv "$XF/" @
pull "delta: small files changed" times \
    "$COPY; echo different > dest/a; echo x > dest/size999; $OLD dest/a dest/size999" -rtv "$XF/" @
pull "whole file (-W) after change" times \
    "$COPY; printf XXXXXXXX | dd of=dest/random-3m bs=1 seek=1500000 conv=notrunc 2>/dev/null; $OLD dest/random-3m" \
    -rtvW "$XF/" @
pull "delta: a few of 3000 files changed" times \
    "'$RSYNC' -rt '$URL/many/' dest >/dev/null 2>&1; for f in d3/f7 d17/f50 d29/f99; do echo changed >> dest/\$f; $OLD dest/\$f; done" \
    -rtv "$URL/many/" @

# --- AmigaDOS limits and progress (M6) ---------------------------------------

# check NAME CONDITION... : passes if the shell test succeeds.
check() {
    name=$1
    shift
    if "$@"; then
        PASS=$((PASS + 1))
        echo "ok   $name"
    else
        FAIL=$((FAIL + 1))
        echo "FAIL $name"
        sed 's/^/     arsync stderr: /' "$WORK/got.err" 2>/dev/null | head -10
    fi
}

rm -rf "$WORK/am"
"$ARSYNC" -rt --amiga-rules "$URL/names/" "$WORK/am" > "$WORK/got.out" 2> "$WORK/got.err"
echo $? > "$WORK/got.rc"
check "amiga rules: exit code 23" [ "$(cat "$WORK/got.rc")" = 23 ]
check "amiga rules: ':' skipped" [ ! -e "$WORK/am/with:colon" ]
check "amiga rules: 40-char name skipped" [ ! -e "$WORK/am/a-name-that-is-forty-characters-long.txt" ]
check "amiga rules: 30-char name kept" [ -f "$WORK/am/exactly-thirty-characters-long" ]
check "amiga rules: case clash keeps first" [ -f "$WORK/am/README" -a ! -e "$WORK/am/Readme" ]
check "amiga rules: clashing dir and contents skipped" [ -f "$WORK/am/Docs/x" -a ! -e "$WORK/am/docs" ]
check "amiga rules: > 2 GiB skipped" [ ! -e "$WORK/am/big-3g" ]
check "amiga rules: each skip reported" [ "$(grep -c '^arsync: skipping' "$WORK/got.err")" = 5 ]
rm -rf "$WORK/am"
"$ARSYNC" -rt --amiga-rules --max-name=107 "$URL/names/" "$WORK/am" > "$WORK/got.out" 2> "$WORK/got.err"
check "--max-name=107 allows long names" [ -f "$WORK/am/a-name-that-is-forty-characters-long.txt" ]

# --progress: same lines as rsync once rates and times are masked.
for side in want got; do
    rm -rf "$WORK/$side"
    if [ $side = want ]; then
        "$RSYNC" --no-inc-recursive -rt --progress "$XF/deep/" "$WORK/$side" > "$WORK/$side.out" 2>/dev/null
    else
        "$ARSYNC" -rt --progress "$XF/deep/" "$WORK/$side" > "$WORK/$side.out" 2>/dev/null
    fi
    tr '\r' '\n' < "$WORK/$side.out" | sed -e 's/ *[0-9.]*[kMG]B\/s/ RATE/' -e 's/[0-9]*:[0-9][0-9]:[0-9][0-9]/TIME/' \
        -e "s|$WORK/$side|DEST|" -e 's/^sent.*/SUMMARY/' -e 's/speedup is .*/SPEEDUP/' > "$WORK/$side.norm"
done
check "--progress output matches rsync" cmp -s "$WORK/want.norm" "$WORK/got.norm" || diff "$WORK/want.norm" "$WORK/got.norm" | sed 's/^/     /'

# Ctrl-C (SIGINT) mid-file: exit code 20 and no temp file left behind.
truncate -s 400000000 "$X/sparse-400m"
rm -rf "$WORK/int"
mkdir -p "$WORK/int"
"$ARSYNC" -t "$XF/sparse-400m" "$WORK/int/" > /dev/null 2> "$WORK/got.err" &
apid=$!
sleep 0.3
kill -INT $apid
wait $apid
irc=$?
rm -f "$X/sparse-400m"
check "SIGINT mid-file exits 20" [ "$irc" = 20 ]
check "SIGINT leaves no temp file" [ -z "$(find "$WORK/int" -name '.*' -type f)" ]

# A small change to a 3 MB file must not resend the file.
rm -rf "$WORK/eff"
mkdir -p "$WORK/eff"
"$RSYNC" -t "$XF/random-3m" "$WORK/eff/f" >/dev/null 2>&1
printf XXXXXXXX | dd of="$WORK/eff/f" bs=1 seek=1500000 conv=notrunc 2>/dev/null
touch -d 2001-01-01 "$WORK/eff/f"
received=$("$ARSYNC" -tv "$XF/random-3m" "$WORK/eff/f" | sed -n 's/.*received \([0-9,]*\) bytes.*/\1/p' | tr -d ,)
if [ -n "$received" ] && [ "$received" -lt 100000 ] && cmp -s "$X/random-3m" "$WORK/eff/f"; then
    PASS=$((PASS + 1))
    echo "ok   delta efficiency (received $received bytes for a 3 MB file)"
else
    FAIL=$((FAIL + 1))
    echo "FAIL delta efficiency (received ${received:-?} bytes for a 3 MB file)"
fi

echo "$PASS passed, $FAIL failed"
[ "$FAIL" -eq 0 ]
