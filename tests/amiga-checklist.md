# Testing arsync on an Amiga (FS-UAE or real hardware)

Things vamos can't check (networking, real Kickstart behaviour) are verified here, one
section per milestone. Report what you see, especially anything that differs from
"Expect".

## Setup

1. On the Linux machine: `make amiga` and `tools/test-daemon.sh`.
   - With FS-UAE's built-in socket emulation (`bsdsocket_library = 1` in the `.fs-uae`
     config), the Amiga shares the host's network, so the default `127.0.0.1` works.
   - With Roadshow/AmiTCP/Miami over an emulated or real network card, run
     `tools/test-daemon.sh 0.0.0.0` and use this machine's LAN address below instead of
     `127.0.0.1`.
2. Copy `build/amiga/arsync` to the Amiga (e.g. a shared FS-UAE directory mounted as a
   drive).
3. Open a Shell. Any stack size is fine; arsync uses its own 64 KiB stack.

Testing on both socket setups is worthwhile at some point: UAE's emulated
`bsdsocket.library` and a real stack such as Roadshow differ in places.

## M1 – module listing

| # | Command | Expect |
|---|---------|--------|
| 1 | `arsync --version` | `arsync 0.0.1, protocol version 32` |
| 2 | `version arsync` | `arsync 0.0.1` (the `$VER` tag) |
| 3 | `arsync rsync://127.0.0.1:8730/` | MOTD line, blank line, then `test` and `private` with their comments |
| 4 | `arsync --port=8730 127.0.0.1::` | Same as 3 |
| 5 | `arsync -vv rsync://127.0.0.1:8730/` | Also prints `opening tcp connection…` and `server protocol 32.0, using protocol 32` |
| 6 | `arsync --no-motd rsync://127.0.0.1:8730/` | Prints nothing, returns normally |
| 7 | `arsync rsync://127.0.0.1:9999/` (nothing listening) | `failed to connect … connection failed`; `echo $RC` → 10 |
| 8 | Stop the daemon with Ctrl-C, run `arsync rsync://192.0.2.1/` (unreachable) and press **Ctrl-C** while it hangs | Returns promptly with an error (`interrupted by user`, `$RC` 20) rather than hanging |
| 9 | Without any TCP/IP stack running (or `bsdsocket_library = 0`) | `no TCP/IP stack available`, `$RC` 10 |

After each run, check that nothing is left behind: no requester, no lost memory
(compare `avail` before and after a few runs).

## M2 – authentication and protocol handshake

Until M3, a successful handshake ends with
`arsync: handshake complete; file list transfer is not implemented yet` and `$RC` 4.

| # | Command | Expect |
|---|---------|--------|
| 1 | `arsync -vv rsync://127.0.0.1:8730/test/ RAM:dest` | `compat flags 0x…, checksum md5, seed …`, then the "handshake complete" line, `$RC` 4 |
| 2 | `arsync rsync://amiga@127.0.0.1:8730/private/ RAM:dest`, type `secret` at `Password:` | Typing is **not echoed**; backspace works; then "handshake complete" |
| 3 | Same, type a wrong password | `@ERROR: auth failed on module private`, `$RC` 5 |
| 4 | Same, press **Ctrl-C** at the `Password:` prompt | Returns promptly, `$RC` 20, console not left in raw mode (typing in the Shell echoes normally afterwards) |
| 5 | `echo secret >RAM:pw` then `arsync --password-file=RAM:pw rsync://amiga@127.0.0.1:8730/private/ RAM:dest` | "handshake complete", no prompt |
| 6 | `setenv RSYNC_PASSWORD secret` then the command from 2 | No prompt, "handshake complete" (then `unsetenv RSYNC_PASSWORD`) |
| 7 | `arsync rsync://127.0.0.1:8730/test/nope RAM:dest` | The server's `link_stat "nope" … No such file or directory` message is shown |

## M3 – listing files

Compare with what `rsync` prints on the Linux machine for the same command (times appear
in each side's local time zone).

| # | Command | Expect |
|---|---------|--------|
| 1 | `arsync rsync://127.0.0.1:8730/test/` | `.`, `hello.txt`, `sub` with `drwxr-xr-x`-style modes, sizes like `4,096`, dates `YYYY/MM/DD HH:MM:SS`; `$RC` 0 |
| 2 | `arsync -r rsync://127.0.0.1:8730/test/` | Also `sub/random.bin` with size `100,000` |
| 3 | `arsync -v -r rsync://127.0.0.1:8730/test/` | `receiving file list ... done` first, then a `sent … received …` / `total size is …` summary |
| 4 | `arsync rsync://127.0.0.1:8730/test/nope` | Server's `link_stat … No such file or directory`, then `arsync error: … (code 23)`, `$RC` 23 |
| 5 | Same as 1 with the Amiga clock/time zone set to something other than GMT (Prefs/Locale) | Times shift by exactly that zone's offset compared with the Linux side in UTC (`TZ=UTC rsync …`). If they're off by **twice** the offset, the `loc_GMTOffset` sign assumption (DESIGN §10) is wrong |
| 6 | A large module (e.g. point the daemon at `/usr/share`) with `-r`, output redirected to `RAM:list` | Completes; note the time and free memory (`avail`) before and after |
