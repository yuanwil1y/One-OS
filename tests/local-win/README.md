# Windows local harness for the socket-based host tests

The `esphome_l2` socket tests — `test_api_client` above all — are written against
POSIX sockets. CI compiles and runs them on Linux. This machine has no
`<sys/socket.h>` or `<arpa/inet.h>`, so before this harness existed they could
not be compiled here at all, and every defect in them surfaced only in CI. That
cost a long series of round-trips while the ESPHome Noise client was being
finished.

`posix_shim.h` declares the socket subset those tests and
`firmware/components/esphome_l2` use; `win_socket_loopback.c` implements it over
an in-process byte pipe. `tools/local/run-host-tests.ps1` builds a generated
copy of `test_api_client.c` against it (two constructs have no Windows
equivalent and are rewritten there: a nested struct designator for the IPv4
address, and `SIGPIPE`).

## Status

The shim is good enough to drive **one** connection end to end, and that is how
the encrypted session was debugged: every handshake step printed and compared on
both sides, which excluded the key schedule as the cause.

`tools/local/run-local-win-test.ps1` compiles and links cleanly. Running it
stalls, and the cause is now identified: **this loopback does not survive
sequential connections.** `test_api_client.c` opens five of them, closing each
listener before the next, and Windows reuses the freed descriptor number for the
new listener while the previous thread still believes it owns that number. The
trace shows exactly that — the plaintext phase's `fd=0`/`fd=2` are handed out
again for the encrypted phase while the earlier thread is still reading.

Fixing it means giving the loopback a per-connection handle rather than a flat
descriptor table, which is more work than the remaining value justifies. The
reduced harness (`local_enc.c`, one connection, two ends in one process) is what
to use instead; it is described in `docs/handover-ledger.md` §4d.

## What it is and is not

- It is a debugging aid for **a single connection**. It lets the encrypted
  session be driven, printed and compared on this machine in seconds instead of
  through CI.
- It is **not** a TCP stack, and it is not a test runner: IPv4 loopback only, no
  DNS beyond the numeric host, no half-close, no flow control beyond sleeping,
  and no correct reuse of descriptor numbers after `close`.
- A test that passes here still has to pass in CI. `tests/host-test-groups.txt`
  and the shell runners remain authoritative.

Set `WIN_SHIM_TRACE=1` to have every socket call printed with its descriptor,
byte count and thread id.

## Why not just skip these tests on Windows

Because that is exactly how the defects were missed. The mirror compiles them
and reports a real failure; it does not skip them.
