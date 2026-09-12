# Windows loopback harness (unfinished)

`test_api_client.c` is written against POSIX sockets. CI compiles and runs it on
Linux; this machine has no `<sys/socket.h>`, so it could not be compiled here at
all and every defect in it showed up only in CI — which cost several long
round-trips while the Noise client was being finished.

This directory supplies an in-process socket loopback so that test can run on
Windows, and `tools/local/run-win-shim-test.ps1` builds a rewritten copy of the
test against it.

**It is not finished.** As of commit `707a4fe`:

- the plaintext half of the test runs on it;
- the encrypted half reaches the Noise handshake and then deadlocks;
- the loopback is not a TCP stack and two known differences remain: `recv` can
  return fewer bytes than were asked for, and it has no flow control beyond
  sleeping when a buffer is full.

Two compile issues also remain in the harness itself, both in
`win_socket_loopback.c`: `select`'s `fd_set` and `<sys/time.h>`'s `timeval` are
not the types the stub headers declare, and `struct addrinfo` is only forward
declared there.

Until those are fixed this is a debugging aid, not a test runner, and
`tools/local/run-host-tests.ps1` deliberately does **not** call it: the mirror
keeps reporting the `test_api_client` compile as a loud failure, which is the
honest state. Nothing here is compiled into firmware.
