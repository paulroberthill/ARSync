# ARSync

`arsync` is an rsync client for AmigaOS 3.x. It copies files from and to any rsync daemon
(rsync 3.0 or later; protocol 30–32) over TCP, using rsync's delta transfer, so updating
a large file with small changes only sends the changes, in either direction.

It's a clean-room implementation written for the Amiga: one process (no `fork()`),
`bsdsocket.library` and `dos.library` called directly, and Ctrl-C honoured throughout.

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

| Option | Meaning |
|---|---|
| `-r`, `--recursive` | recurse into directories |
| `-t`, `--times` | preserve modification times (needed for fast re-runs: without it every file is re-checked by content) |
| `-a`, `--archive` | same as `-rt` here (links, permissions and owners aren't supported) |
| `-n`, `--dry-run` | show what would be transferred |
| `-v`, `--verbose` | list transferred files and a summary; `-vv` for more |
| `-P`, `--progress` | show progress for each file |
| `-W`, `--whole-file` | don't use delta transfer (can be quicker with a slow CPU and a fast network) |
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

Modification times are converted to local time using the Locale preferences' time zone.
Times before 1978 (the AmigaDOS epoch) are stored as 1 Jan 1978.

When sending, files get permissions 0644 and directories 0755 on the server (subject to
its umask); AmigaDOS protection bits aren't translated. Soft links are listed but, as with
rsync without `-l`, not copied.

## Not supported

`--delete`, include/exclude rules, compression (`-z`), symlinks, permissions/owners, ssh
transport. `--delete`, excludes and `-z` are planned.

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

MIT; see `LICENSE`. No rsync source code is included.
