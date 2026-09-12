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

The shim itself works: the encrypted session was driven to completion on this
machine, every handshake step printed and compared on both sides, and the three
client defects it exposed were fixed.

`tools/local/run-local-win-test.ps1` **compiles and links cleanly**, and running
it reproduces the failure in seconds. As of this commit it stalls in the
plaintext phase before reaching the encrypted one: both threads end up blocked
in `recv` (`fd=1 n=1` on one side, `fd=2 n=1` on the other), which is a
difference between this loopback and real TCP that has not been identified yet.
Set `WIN_SHIM_TRACE=1` to see it.

That stall does **not** block debugging the encrypted session: a standalone
harness that runs both ends in one process over the same shim
(`local_enc.c`, described in `docs/handover-ledger.md` §4d) reaches the
handshake and reports the exact divergence.

## What it is and is not

- It is a debugging aid. It lets the encrypted session be driven, printed and
  fixed on this machine in seconds instead of through CI.
- It is **not** a TCP stack: IPv4 loopback only, no DNS beyond the numeric host,
  no half-close, no flow control beyond sleeping when a buffer is full.
- A test that passes here still has to pass in CI. `tests/host-test-groups.txt`
  and the shell runners remain authoritative.

Set `WIN_SHIM_TRACE=1` to have every socket call printed with its descriptor,
byte count and thread id.

## Why not just skip these tests on Windows

Because that is exactly how the defects were missed. The mirror compiles them
and reports a real failure; it does not skip them.
