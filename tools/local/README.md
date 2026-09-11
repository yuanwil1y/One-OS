# Local host test entry point (Windows)

CI runs `tests/run_all_host_tests.sh`, which invokes the per-group runners listed in
`tests/host-test-groups.txt`. Those runners are POSIX shell. A Windows machine without
a POSIX shell that has a C compiler cannot execute them, which previously meant a
developer had no way to run the regression suite before pushing.

`run-host-tests.ps1` closes that gap: it performs the **same compile and run steps** as
each runner, with the **same flags, sources, include directories and environment**, so a
local pass means what a CI pass means.

## What is authoritative

The manifest, the per-group `.sh` runners and CI remain the source of truth. This script
is a developer convenience, is not referenced by the build, and adds no group of its own —
`-List` reads the same manifest CI reads.

Adding a test group means adding a runner and one manifest line, **and** one `switch` case
here. A group with no local case fails loudly rather than being skipped:

```
group 'x' has no local equivalent; add one to tools/local/run-host-tests.ps1
```

## Toolchain

A MinGW-w64 clang with `-fsanitize=address,undefined` is required, matching the
sanitizer coverage CI gets from GCC on Linux. `llvm-mingw` provides both, and needs no
Visual Studio install:

```
winget install LLVM.LLVM            # optional: shares clang-format and clangd
# llvm-mingw is discovered from D:\OS\.toolchain, or set DSH_LOCAL_TOOLCHAIN
$env:DSH_LOCAL_TOOLCHAIN = 'C:\path\to\llvm-mingw-<version>-ucrt-x86_64'
```

## Usage

```powershell
powershell -File tools/local/run-host-tests.ps1              # every group
powershell -File tools/local/run-host-tests.ps1 -Group app_device_db
powershell -File tools/local/run-host-tests.ps1 -List
```

## Groups that cannot run locally

Two groups build POSIX socket code (`arpa/inet.h`, `sys/socket.h`, `netdb.h`), which
llvm-mingw's sysroot does not provide:

| Group | Why | Where it is verified |
|---|---|---|
| `esphome_l2` | `test_api_client.c` uses `arpa/inet.h` and `pthread` | CI (`ubuntu-latest`) |
| `nmap_l2` | `nmap_core.c` and its test use `arpa/inet.h` | CI (`ubuntu-latest`) |

They fail at **compile** time with "file not found" for a system header, which is
unmistakably an environment limitation and never a silent skip. Both pass in CI.

## Deliberate differences from the runners

1. **Fixture path.** `run_app_device_db_tests.sh` and `run_device_db_tests.sh` pass
   `-DDEVICE_DB_FIXTURE_DIR=<absolute path>`. A Windows path cannot be quoted portably
   through `-D`, so this script omits it; the two test programs then derive the directory
   from their own `__FILE__`. The affected tests were changed to make the `-D` optional
   for exactly this reason, and a directory that cannot be found **fails** the run rather
   than skipping it.
2. **`wireshark_l2`.** The runner uses `make`; this script issues the equivalent `clang`
   command directly, with the same sources and include directories.
3. **`zha_zigpy_l2` boundary checks.** The runner uses `grep -R`; this script uses
   `Select-String` with the same patterns over the same directories.

Everything else is the same command CI runs.
