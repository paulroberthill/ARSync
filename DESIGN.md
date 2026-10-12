# ARSync — design

Status: draft, 2026-10-12. Builds on `FEASIBILITY.md`.

ARSync is a minimal, clean-room rsync **client** for AmigaOS 3.x. It talks to unmodified
rsync daemons (rsync 3.x) using **protocol 32**. Upstream rsync 3.4.1 source is the
protocol reference only — no upstream code is copied (keeps the licence choice open).

---

## 1. Scope

### In scope (v1)

| Feature | Notes |
|---|---|
| Transport: rsync daemon over TCP (`rsync://host[:port]/module/path`, `host::module/path`) | `bsdsocket.library`, called directly. A single colon (`Work:dir`) always means a local AmigaDOS path; there is no remote-shell form |
| Module listing (`rsync://host/`) | Prints MOTD + module list |
| Daemon authentication | Challenge-response using the strongest digest both sides know (md5 → md4); password from `--password-file` or `RSYNC_PASSWORD` |
| **Pull** (server → Amiga) | First real transfer mode |
| Recursive (`-r`), times (`-t`), dry run (`-n`), list only (`--list-only`), verbose (`-v`), `--port` | |
| Delta transfer against existing local files | md5 strong sums, rolling checksum |
| Whole-file transfer (`-W`) | Also the automatic path when no local file exists |
| Ctrl-C to abort cleanly | Removes the temp file and closes the socket |
| Progress/stats (`--progress`, `--stats` basic) | |

### Later (v2+), in rough order

1. **Push** (Amiga → server). Concurrency-wise this is actually simpler than pull (the
   upstream sender is single-process), so it may move up.
2. `--delete` (receiver-side deletion, pull only).
3. `--exclude` / `--include` / `--exclude-from` (filter list sent over the wire).
4. `-z` compression (`zlib` only; zstd/lz4 not planned).
5. Permission mapping (`-p`) approximated onto RWED bits.
6. `rsync-ssl` (TLS daemon access) via AmiSSL.

### Explicitly out of scope

ssh / remote-shell transport, acting as a daemon or server, `-l`/`-H` (links), `-o`/`-g`
(owners), devices/specials, ACLs, xattrs, `--inplace`, `--partial-dir`, `--fuzzy`, batch
files, incremental recursion, xxh*/sha checksums, zstd/lz4.

---

## 2. Platform targets

- **Primary:** AmigaOS 3.x (3.1 minimum, 3.2 and 3.9 tested), 68020+ CPU, Fast RAM
  recommended. A plain 68000 build is possible but won't be tuned.
- **TCP stack:** any `bsdsocket.library` (Roadshow, AmiTCP, Miami).
- **Toolchain:** bebbo amiga-gcc 6.5.0b at `/opt/amiga`, built with `-mcrt=nix20 -m68020`
  (libnix). Note that this GCC's default runtime is newlib, not clib2: with no `-mcrt`
  flag the specs pick newlib. libnix was chosen over newlib because:
  - binaries are about 3× smaller (22 KB vs 72 KB for the M0 `arsync`);
  - it doesn't pull in `mathieeedoubbas.library`;
  - its argv handling works under vamos (newlib's comes back empty there).

  CLI program, no GUI in v1.
- **Stack:** `plat_run()` always moves `main` onto a private 64 KiB stack with
  `StackSwap()`. It doesn't rely on a `__stack` cookie, which the newlib startup ignores.
  Verified under vamos (8 KiB → 64 KiB and back).
- **Host build (development):** the same sources build natively on Linux with a POSIX
  platform layer. All protocol work is developed and tested on the host against a local
  `rsync --daemon` before any Amiga testing. This is the main lever for development
  speed, since the Amiga can't run on the dev machine.

---

## 3. Architecture

```
            +--------------------------------------------------+
  CLI ----> |  main / options                                  |
            +--------------------------------------------------+
            |  session: daemon handshake, auth, server args     |
            +--------------------------------------------------+
            |  engine (single-process state machine)            |
            |   generator  <-- internal queues -->  receiver    |
            |   flist (recv/store)    sums    delta apply       |
            +--------------------------------------------------+
            |  wire: multiplexed I/O, varint/varlong, buffers   |
            +--------------------------------------------------+
            |  crypto: md4, md5, rolling checksum               |
            +--------------------------------------------------+
            |  platform (plat_*):  net | fs | time | signal     |
            |    plat_amiga.c (dos.library + bsdsocket)         |
            |    plat_posix.c (Linux host build)                |
            +--------------------------------------------------+
```

### Modules

| File | Responsibility |
|---|---|
| `main.c`, `options.c` | Parse CLI, build the request, exit codes (match rsync's where meaningful) |
| `session.c` | Connect, greeting, auth, module list, build and send server args |
| `wire.c` | Buffered socket I/O, multiplex framing in/out, `read/write_{byte,int,varint,varlong,vstring}` |
| `flist.c` | Decode file-list entries, compact in-memory storage, sorting/dedup like upstream |
| `generator.c` | Decide per file: skip / whole / delta; produce block sums from the basis file |
| `receiver.c` | Consume token stream, rebuild file into temp, verify whole-file md5, rename, set time |
| `checksum.c`, `md4.c`, `md5.c` | Rolling checksum (`get_checksum1`), strong sums, whole-file sum, seed handling |
| `fsmap.c` | Map remote names/times/modes onto AmigaDOS (see §6) |
| `plat_amiga.c`, `plat_posix.c` | OS abstraction: sockets, open/read/write/close, stat, mkdir, rename, delete, set date, set protection, dir scan, break check |

### Task model: one process, cooperative state machine

Upstream pull uses two processes (generator + receiver) sharing one socket, and that's
where `fork()` comes in. ARSync uses **one process** instead:

- One non-blocking socket. A main loop waits on `WaitSelect()` (Amiga) / `select()`
  (host) for readable/writable **plus** `SIGBREAKF_CTRL_C`.
- **Receiver side** runs whenever input is available: demultiplex the frames, feed
  `MSG_DATA` bytes into the receiver state machine, and handle `MSG_INFO`/`MSG_ERROR`/
  `MSG_IO_ERROR`, etc.
- **Generator side** runs whenever the output buffer has room: emit the next file request
  (index + item flags + sum header + block sums), then the phase markers.
- The two halves only talk through in-memory queues (e.g. receiver → generator "redo
  this index" for phase 2, which upstream sends as `MSG_REDO` over a pipe).
- Rule: **never block on write while unread input is pending**, and vice versa. This
  avoids the classic rsync deadlock where both sides fill their socket buffers.
  Block-sum generation is chunked, so a large basis file can't starve the reader.

Why not `CreateNewProc()` with two tasks: it would work (no MMU), but debugging shared
state between two Amiga processes without memory protection is painful, and the same
design must also run on the Linux host. A single state machine is deterministic and
testable on the host.

---

## 4. Protocol flow (pull, protocol 32)

Checked against rsync 3.4.1 `clientserver.c`, `compat.c`, `flist.c`, `io.c`.

1. **Greeting.**
   - Client sends: `@RSYNCD: 32.0 md5 md4\n`. The trailing list is the *auth digest*
     list; it's mandatory for protocol > 31.
   - Server replies: `@RSYNCD: 32.0 <digest list>\n`, optionally followed by MOTD lines.
   - Negotiated version = min(ours, theirs), lowered by one if the subprotocols differ.
     Accept 30–32. Protocol ≥ 30 means daemon args are sent NUL-terminated.
2. **Module request.**
   - Client sends `module\n` (or an empty line for a listing).
   - The server then sends one of: `@RSYNCD: AUTHREQD <challenge>`, `@RSYNCD: OK`,
     `@RSYNCD: EXIT` (end of a module list), or `@ERROR ...`.
3. **Auth.**
   - Response: `user base64(digest(password + challenge))`.
   - The digest is the first one in the server's list that we support.
4. **Server args.** Sent as NUL-separated strings, terminated by an empty string. For a
   pull this looks like:
   `--server --sender -rte.fxCIv . module/path`
   - `-e.<letters>` is the client-capability string ("client_info"):
     - `f` — safe flist / I/O-error end marker.
     - `x` — avoid the xattr optimisation.
     - `C` — fixed checksum-seed order.
     - `I` — inplace/partial-dir compatibility.
     - `v` — enables negotiated strings and varint flist flags.
     - **No `i`**, so the server won't use incremental recursion.
     - **No `L`/`s`**, since we don't handle symlinks.
   - Exact letters will be confirmed against a live daemon with `--debug=proto,nstr`.
5. **Compat flags.** The server sends a varint of `CF_*` bits. Check:
   - `CF_VARINT_FLIST_FLAGS` is set (flist flags are sent as varints).
   - `CF_INC_RECURSE` is clear.
   - Note `CF_CHKSUM_SEED_FIX`.
6. **Negotiated strings.**
   - Client sends the checksum list as a vstring: `"md5 md4"`. A compression list is
     only sent with `-z`.
   - Read the server's list; the first entry in *our* list that the server also has wins.
7. **Checksum seed.** Read an int32 from the server.
8. **Multiplexing.** From here on, everything from the server arrives in frames:
   - 4-byte little-endian header = `((MPLEX_BASE + tag) << 24) | length`, where
     `MPLEX_BASE` = 7.
   - `tag` 0 = `MSG_DATA`; the others are messages.
   - Our own output is also multiplexed for protocol ≥ 30.
9. **Filter list.** Client (as receiver) sends its exclude list. v1 sends an empty list
   (int32 0).
10. **File list.** Read entries until the flags varint is 0, then the I/O-error value.
    Each entry, in order:
    - **flags:** varint (`XMIT_*`).
    - **name:** a byte giving the prefix length shared with the previous name (if
      `SAME_NAME`), then the suffix length (varint if `LONG_NAME`, otherwise a byte),
      then the suffix bytes.
    - **size:** varlong(3).
    - **mtime:** varlong(4), unless `SAME_TIME`.
    - **nsec:** varint, if `MOD_NSEC`.
    - **mode:** int32, unless `SAME_MODE`.
    - uid/gid, rdev, symlink target, hardlink data and checksum aren't sent, because we
      don't request `-o -g -D -l -H -c`.

    Then sort and clean the list exactly as upstream does: the indices must match the
    sender's.
11. **Transfer phases.**
    - **Generator, per regular file needing an update:**
      - Write the index (`write_ndx` delta encoding) and the item flags (`ITEM_TRANSFER`).
      - Write the sum header (`count`, `blength`, `s2length`, `remainder`). It's
        `count`=0 when there's no basis, which gives a whole-file transfer.
      - Write `count` × (rolling sum int32 + `s2length` bytes of md5).
    - **Receiver, per file:**
      - Read the index, item flags and sum header echo.
      - Read tokens: positive int = that many literal bytes follow; negative int = copy
        block `-(n+1)` from the basis; 0 = end.
      - Then read the whole-file md5 (16 bytes).
      - Verify the checksum. On a mismatch, queue a redo for phase 2 (full-length
        strong sums). Otherwise rename the temp file over the target and set its mtime.
    - **Phases:** `NDX_DONE` is exchanged to end phase 1, phase 2 (redo), and the final
      phase; then the stats exchange and goodbye.
12. **Exit.** Close the socket. The exit code follows rsync's (0, 23 for a partial
    transfer, 5/10/12 for start-up/socket/protocol errors, 20 for a signal).

Block size: upstream's `sum_sizes_sqroot()` rule (√size rounded to a multiple of 8,
min 700, max 128 KiB). `s2length` follows upstream's formula, so the server accepts the
sums.

---

## 5. Data structures and memory

- **File-list entry (≈ 24 bytes + name):** offset into a name pool, size (int64), mtime
  (int32), mode (uint16), flags. Names go into a growing string pool; a "dirname
  shared" trick like upstream's keeps long trees compact.
- **Budget target:** 10,000 files in < 1 MB.
- I/O buffers: 32 KiB in, 32 KiB out (`CHUNK_SIZE`).
- Basis sums: up to ~(4 + 16) bytes × block count. A 50 MB file with 7 KiB blocks
  needs ≈ 140 KB. These are generated streaming and never held for more than one file.
- Receiver: the rebuild reads basis blocks through a small cache and writes the temp
  file sequentially.
- All allocations go through one wrapper that turns failure into a clean "out of
  memory" exit (exit code 22), never a crash.

---

## 6. AmigaDOS mapping (`fsmap.c`)

| Remote | Amiga handling |
|---|---|
| `/` path separators | Kept; AmigaDOS uses `/` too. A leading `/` is forbidden. `..` components are rejected (sanitised like upstream) |
| Name too long for the target volume | Ask the filesystem for its limit at start; skip the entry with a warning (no silent truncation) |
| Names differing only by case | Second one gets a warning and is skipped |
| Characters invalid on AmigaDOS (`:`) | Skipped with a warning |
| UTF-8 names | v1: bytes passed through unchanged. v2: optional UTF-8 → Latin-1 (`--iconv`-like flag) |
| mtime (Unix seconds) | Amiga `DateStamp` = Unix − 252,460,800 s (1978 epoch); ticks = nsec / 20 ms. Pre-1978 times clamp to 1978-01-01 |
| Size > 2 GiB | Entry skipped with a warning (OS3 dos.library limit) |
| Mode | v1: ignored (default RWED). v2 `-p`: owner r/w/x → R/W/E, D = W |
| Symlinks, devices, specials | Not requested from the server; nothing to map |

When comparing local vs remote mtimes (quick check: size + mtime), allow a ±1 s window
(the `--modify-window` default for FAT-like resolution), because AmigaDOS stores 1/50 s.

---

## 7. Development and testing

- **Host build first:** `make host` builds `arsync` for Linux; `make amiga` cross-builds.
  The only per-target files are `plat_*.c`.
- **Reference server:** the system rsync 3.2.7 (protocol 32) runs as a local daemon from
  a scratch `rsyncd.conf`. Server-side `--debug=proto,nstr,flist,deltasum` shows what
  it saw from us.
- **Golden tests:** pull a fixture tree into an empty dir and into a pre-seeded,
  modified copy, then `diff -r` against the source. Cover:
  - empty files, a 0-length dir, 1-byte files, exactly-one-block files, a 100 MB file;
  - long names and names with spaces;
  - many small files.
- **Unit tests (host):** varint/varlong round-trips, md4/md5 test vectors, rolling
  checksum against upstream values, multiplex framing.
- **68k unit tests without an emulator:** `make test-amiga` runs the cross-built test
  binary under vamos (amitools, set up by `tools/setup-vamos.sh` with pinned versions).
  That's real big-endian 68020 code against emulated `exec`/`dos.library`. It catches
  endianness and dos.library mistakes on the dev machine, but has no TCP/IP stack.
- **Amiga testing:** FS-UAE run by the user. Each milestone ships a binary plus a short
  test script, and black-box checks (`strings`, see global notes) are done before
  handing over.

---

## 8. Milestones

| # | Deliverable | Proves |
|---|---|---|
| M0 | Skeleton, both builds, `plat_*`, wire primitives + unit tests | Toolchain and abstraction |
| M1 | `arsync rsync://host/` lists modules | Sockets, greeting, version negotiation |
| M2 | Auth + server args + compat flags + string negotiation + seed | Full protocol-32 handshake |
| M3 | `--list-only rsync://host/mod/path` | Multiplex reader, file list decoding, sorting |
| M4 | Pull, whole-file only (`count`=0) | Generator/receiver state machine, phases, md5 verify |
| M5 | Delta pull against existing files | Block sums, token stream, redo phase |
| M6 | AmigaDOS mapping, Ctrl-C, progress/stats, first Amiga release | Real-hardware behaviour |
| M7 | Push | Sender role |
| M8 | `--delete`, excludes, `-z` | v2 features |

M1–M3 can be tested on the Amiga early, since they need very little filesystem code.

---

## 9. Open decisions (defaults assumed until changed)

1. **Binary name:** `arsync` (CLI, rsync-compatible option spelling).
2. **Minimum CPU:** 68020 build only; optimisation for 68030/040/060 later.
3. **Licence:** clean-room, so free choice. Default suggestion: MIT or BSD-2.
4. **Push vs pull first:** pull first, as stated in the feasibility doc. Push is easier
   to implement, so swap them if pushing from the Amiga matters more to you.

## 10. Assumptions to verify on a real Amiga / FS-UAE

- **`loc_GMTOffset` sign.** It's taken as minutes *west* of GMT (local = UTC − offset),
  matching AROS's interpretation. If transferred files show times off by twice the
  zone offset, flip the sign in `amitime.c`.
- **`StackSwap()` on real Kickstart.** It works under vamos; check once on FS-UAE.
- **bsdsocket behaviour** (non-blocking I/O, `WaitSelect()` with Ctrl-C, `Errno()`
  values). It can't be exercised under vamos, so it's first tested at M1 on FS-UAE.

## 11. Deliberate differences from rsync's client (so far)

- **`@RSYNCD: EXIT` after a module request** (rather than after a listing request): rsync
  silently exits 0; arsync reports "server ended the session" and exits 5.
- **Password files aren't checked for permissions:** AmigaDOS has no owner or other bits.
  rsync refuses files that others can read.
- **No `-e.i`, `L` or `s`:** see §4. If a server still turns on incremental recursion,
  arsync stops with a protocol error.
- **No incremental recursion:** with `-r`, rsync's own client prints `receiving incremental
  file list`. arsync always gets the whole list first and prints `receiving file list ...
  done`, and its `-v` byte counts match `rsync --no-inc-recursive`.
- **`-a` means `-rt`:** the rest of rsync's `-a` (`-lpgoD`: links, permissions, owner, group,
  devices) isn't supported.
- **Temp files:** rsync names them `.NAME.XXXXXX` next to the target; arsync does the same
  but shortens NAME so the whole name fits FFS's 30-character limit.
- **Protocol 30/31 servers without varint file-list flags** (rsync 3.0.x–3.1.x) skip
  checksum negotiation and use md5. The file-list decoder (M3) must also handle their
  older flag encoding.

## 12. Implementation notes (M4)

- **One reply per request:** each request we send (a transfer, or a changed item such as a
  new directory) gets exactly one reply in index order, or a `MSG_NO_SEND` when the server
  can't open the file. `MSG_NO_SEND` can arrive in the middle of another file's data, so it
  is only counted when it arrives. Only a wait for the *start* of the next reply is cut
  short by it.
- **Flush before every wait:** the connection calls a hook that flushes the multiplexer
  before waiting for input. Without it, requests buffered in a not-yet-full frame were
  never sent and the pull hung (found in testing).
- **Delta transfer (M5):** for an existing local file the request carries its block
  checksums, with block size and checksum length computed exactly as rsync does. Phase 1
  uses short strong checksums and the redo phase full 16-byte ones. The receiver copies
  referenced blocks from the old file through a 256 KiB read window. `-W` sends whole
  files.
- **Backpressure:** with large checksum sets and large replies in flight together, blocking
  on a write while the input buffer is full could deadlock. So when input is full, writes
  stay buffered in memory, and the generator only runs while our output isn't backed up.
  Stress-tested with 30 × 4 MB files changing at once.
- **Compiler bug:** GCC 6.5 for m68k miscompiles `while (v >>= 1)` on a 64-bit `v` at
  `-O2`. The loop tests the Z flag of the low word's `roxr` only. `checksum.c` therefore
  computes `ilog2` on 32-bit halves. Don't write 64-bit shift-then-test-for-zero loops in
  this code base.

