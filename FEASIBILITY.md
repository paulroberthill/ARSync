# ARSync — rsync client for AmigaOS 3.x: feasibility notes

Date: 2026-10-11. Target: rsync protocol 32 (rsync 3.4.x), GCC (bebbo amiga-gcc 6.5.0b, clib2).

## Verdict

Feasible, moderate-to-large effort. Protocol is not the hard part; the hard parts are
rsync's reliance on `fork()`, 68k checksum speed, and AmigaDOS-vs-POSIX filesystem
semantics.

## Toolchain status (checked)

- `/opt/amiga/bin/m68k-amigaos-gcc` = GCC 6.5.0b, clib2 runtime.
- clib2 provides `sys/socket.h`, `netdb.h`; NDK has `proto/bsdsocket.h`,
  `inline/bsdsocket.h`, `libraries/bsdsocket.h`.
- `_NO_INLINE` / ReAction notes in global CLAUDE.md only matter if a GUI is added.

## Why it's feasible

- Protocol 30+ negotiation lets the client advertise a minimal subset:
  - checksums: `md5` (fallback `md4`) — no need for xxh128/xxh3/xxh64
  - compression: `none` initially, `zlib` later; skip zstd/lz4
  - incremental recursion is opt-in via `-e.xxx` compat flags — can be omitted at first
- Daemon transport (`rsync://host/module`, TCP 873) needs only `bsdsocket.library`
  (Roadshow/AmiTCP/Miami). Daemon auth = simple MD4/MD5 challenge-response.
- varint/varlong encoding, multiplexed I/O, file list, rolling checksum, delta
  reconstruction: all plain C on clib2; GCC 6.5 handles int64.

## Obstacles (hardest first)

1. **No `fork()`** in clib2. Upstream forks generator + receiver (and ssh/local
   transports). Restructure as either two `CreateNewProc()` tasks sharing memory (no MMU
   protection on OS3) or a single-process `WaitSelect()`-driven loop.
2. **CPU/memory.** MD5 on a 68030 ≈ 1–2 MB/s; need 68020+ with Fast RAM realistically.
   File list held in memory (~100+ bytes/file). Default 4 KB stack too small — add a stack
   cookie.
3. **Filesystem mismatch** (map or ignore):
   - 32-bit file sizes in OS3 dos.library → no files > 2 GB
   - filename length 30 (OFS/FFS), ~107 (FFS2-long/PFS3/SFS)
   - case-insensitive names → `Foo`/`foo` collisions from server
   - timestamp epoch 1978, 1/50 s resolution
   - RWED protection bits ≠ POSIX uid/gid/mode → `-p/-o/-g` approximate at best
   - soft links unreliable on FFS, hard links impractical → skip `-l/-H` initially
   - Latin-1 local names vs UTF-8 servers → charset conversion
4. **ssh transport.** No well-maintained OpenSSH client for OS3 known, and no fork/exec
   plumbing. Go daemon-only first; `rsync-ssl` (TLS daemon) via AmiSSL is a more
   realistic secure option than ssh.

## Approaches

| | Port upstream rsync 3.4.x | Clean-room minimal protocol-32 client |
|---|---|---|
| Compatibility | Full option set, proven code | Chosen subset only |
| Effort | Large: fork() rework across main/receiver/generator + POSIX shim | Medium: single-process design for AmigaDOS |
| Licence | GPLv3 (port must be GPLv3) | Own choice |
| Risk | POSIX assumptions everywhere | Protocol edge cases |

**Recommendation:** clean-room minimal daemon-mode client — pull first, then push; md5,
no compression; recursion, times, sizes. Use upstream source as protocol reference. Add
zlib, `--delete`, push incrementally.

## Open items / next steps

- Search Aminet for older ixemul-based rsync 2.x ports (unverified whether any exist) —
  could show how others handled `fork()`.
- Next: short design doc (scope, supported options, task model).
