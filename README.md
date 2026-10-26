# ARSync

`arsync` is an rsync client for AmigaOS 3.x. It copies files from and to any rsync daemon
(rsync 3.0 or later; protocol 30–32) over TCP, using rsync's delta transfer, so updating
a large file with small changes only sends the changes, in either direction.

It's a clean-room implementation written for the Amiga: one process (no `fork()`),
`bsdsocket.library` and `dos.library` called directly, and Ctrl-C honoured throughout.

**Written by an AI.** The code, tests and documentation were written by Claude (Anthropic's
AI model) in Claude Code, directed and tested on the Amiga by Paul Hill. Try it on
copies of your data first, and use `-n` before `--delete`. See `dist/arsync.readme` for
the version history.

## Requirements

- AmigaOS 3.1 or later, 68020 or better (Fast RAM recommended)
- A TCP/IP stack providing `bsdsocket.library`: Roadshow, AmiTCP, Miami, or an
  emulator's built-in one (e.g. FS-UAE `bsdsocket_library = 1`)
- An rsync daemon to talk to (`rsync --daemon`, or any public rsync mirror)

No particular Shell stack size is needed: arsync switches to its own 64 KiB stack.

## Usage

```
arsync rsync://HOST/                         list the daemon's modules
arsync rsync://HOST/MODULE/PATH              list files
arsync -rtv rsync://HOST/MODULE/PATH Work:Dest   fetch a directory tree
arsync -t HOST::MODULE/file Work:file         fetch one file
arsync -rtv Work:Projects/ rsync://HOST/MODULE/backup   send a directory's contents
arsync -t Work:file HOST::MODULE/              send one file
```

Sending needs a module the daemon lets you write to (`read only = no` in
`rsyncd.conf`).

Locations can be `rsync://[USER@]HOST[:PORT]/MODULE[/PATH]` or `[USER@]HOST::MODULE[/PATH]`.
A path with a single colon (`Work:Dest`) is always local. As with rsync, a trailing `/` on
the source means "the contents of this directory"; a volume or assign name (`Work:`) means
its contents too.

So to back up a whole volume and restore it, put the `/` on the server path when restoring;
without it the restore creates an `Apps` directory inside `Apps:`:

```
arsync -rtv --delete Apps: rsync://HOST/MODULE/Apps      back up Apps:
arsync -rtv --delete rsync://HOST/MODULE/Apps/ Apps:     restore it (note the trailing /)
```

Add `-n` to the restore first: with `--delete` it removes anything on `Apps:` that isn't in
the backup.

| Option | Meaning |
|---|---|
| `-r`, `--recursive` | recurse into directories |
| `-t`, `--times` | preserve modification times (needed for fast re-runs: without it every file is re-checked by content) |
| `-a`, `--archive` | same as `-rt` here (links, permissions and owners aren't supported) |
| `-n`, `--dry-run` | show what would be transferred |
| `-v`, `--verbose` | list transferred files and a summary; `-vv` for more |
| `--progress` | show progress for each file |
| `--partial` | if a transfer is cut short (Ctrl-C, lost connection), keep what arrived; the next run then only fetches the rest |
| `-P` | same as `--partial --progress` |
| `-i`, `--itemize-changes` | list each change with rsync's change codes (`>f.st...... name`); `-ii` lists unchanged items too |
| `-u`, `--update` | don't replace files that are newer on the destination |
| `--existing` | only update files that already exist on the destination; create nothing new |
| `--ignore-existing` | only create new files; leave existing ones alone |
| `--size-only` | treat files of the same size as up to date, whatever their times |
| `-c`, `--checksum` | compare files by a checksum of their contents instead of size and time (reads every file on both sides, so slow on a 68k, but finds changes that kept size and time) |
| `--stats` | show rsync's transfer statistics at the end (files, created, deleted, literal/matched data, bytes) |
| `-W`, `--whole-file` | don't use delta transfer (can be quicker with a slow CPU and a fast network) |
| `-z`, `--compress` | compress file data in transit (worth it on slow links; costs CPU) |
| `--compress-level=N` | zlib level 1–9 (default 6) |
| `--zc=zlib\|zlibx` | compression method (default: agreed with the server) |
| `--delete` | delete destination files that aren't on the source side |
| `--delete-excluded` | also delete excluded files on the destination (implies `--delete`) |
| `--exclude=PATTERN` | skip files matching PATTERN |
| `--include=PATTERN` | don't skip files matching PATTERN (put it before a wider `--exclude`) |
| `--exclude-from=FILE`, `--include-from=FILE` | read patterns from FILE, one per line (lines starting with `#` or `;` are ignored) |
| `--filter=RULE` | a rule like `- *.info` or `+ docs/`; `!` clears the rules so far |
| `--list-only` | list files instead of copying |
| `--password-file=FILE` | password for modules that need one (or set `RSYNC_PASSWORD`, or type it when asked) |
| `--port=PORT` | daemon port for `HOST::` locations (default 873) |
| `--timeout=SECS` | give up if nothing arrives for this long |
| `--no-motd` | don't show the daemon's message of the day |
| `--max-name=N` | longest file name the destination accepts (default: detected from the volume) |

Exit codes are rsync's: 0 success, 5 couldn't start (e.g. unknown module, wrong
password), 10 connection error, 20 interrupted (Ctrl-C), 23 some files not transferred,
and so on.

## How AmigaDOS limits are handled

Files the destination can't hold are **skipped with a warning** (exit code 23), never
renamed or truncated, since a renamed file would look new on every run:

- names containing `:`;
- names longer than the volume allows. That's 30 characters on FFS/OFS. Long-name FFS,
  PFS and SFS volumes are detected and allow 107. Use `--max-name` to override;
- names that differ only in case from another name in the same transfer (the first, in
  rsync's order, is kept; a skipped directory's contents are skipped too);
- files of 2 GiB or more.

Modification times are converted to local time. AmigaOS 3.x has no daylight saving, so if
your clock changes for summer time, give arsync the rule in `ENV:TZ` in POSIX form, and
each file's time is converted with the offset for its own date. For the UK:

```
SetEnv TZ GMT0BST,M3.5.0/1,M10.5.0
Copy ENV:TZ ENVARC:
```

(Central Europe: `CET-1CEST,M3.5.0,M10.5.0/3`; US Eastern: `EST5EDT,M3.2.0,M11.1.0`.)
Without `ENV:TZ`, arsync uses `ENV:TZONE` (hours west of Greenwich, as set for SNTP; `-1`
is UTC+1), and failing that the Locale preferences' time zone. Both are single offsets, so
files from the other half of the year come out an hour off, and changing the offset makes
every file's time look changed on the next run.
Times before 1978 (the AmigaDOS epoch) are stored as 1 Jan 1978.

When sending, files get permissions 0644 and directories 0755 on the server (subject to
its umask); AmigaDOS protection bits aren't translated. Soft links are listed but, as with
rsync without `-l`, not copied.

## Excludes and `--delete`

Patterns work as in rsync: `*`, `?` and `[...]` match within a name, `**` across `/`; a
pattern with a `/` is matched against the path from the transfer's top, and a leading `/`
anchors it there; a trailing `/` matches only directories. The first matching rule wins.
For example, `--include='*/' --include='*.mod' --exclude='*'` copies only `.mod` files.

`--delete` removes files on the destination that the source doesn't have, so the two end
up the same. When pulling, arsync does the deleting (and says `deleting NAME` with `-v`);
when sending, the server does. Excluded files are left alone unless you add
`--delete-excluded`. Try it with `-n` first. If the server couldn't read some of its
files, arsync deletes nothing, as rsync does.

When sending with `-z`, files that are already packed (`.lha`, `.lzx`, `.dms`, `.zip`,
`.gz`, `.jpg`, `.mp3` and the like) are sent uncompressed.

## Not supported

Symlinks, permissions/owners, ssh transport.

## Building

Cross-compiled on Linux with bebbo's amiga-gcc (`/opt/amiga`):

```
make                # Linux and Amiga builds of arsync and the unit tests
make test           # unit tests on Linux
make test-amiga     # unit tests as 68k code under vamos (run tools/setup-vamos.sh once)
make itest          # integration tests against a local rsync daemon
make release        # build/release/arsync-VERSION.lha
```

The same sources build natively on Linux, which is how most testing is done. Integration
tests compare arsync's output, exit codes and resulting files with the real rsync client's.
`tools/test-daemon.sh` runs a daemon for testing from an emulated Amiga, and
`tests/amiga-checklist.md` lists what to check there.

See `DESIGN.md` for how it works and how it differs from rsync.

## Licence

MIT; see `LICENSE`. No rsync source code is included. The Amiga build includes zlib 1.3.1
(`third_party/zlib`, zlib licence).
