<#
.SYNOPSIS
  Run a socket-based host test on Windows, on the in-process loopback.

.DESCRIPTION
  See tests/local-win/README.md. This builds a generated copy of the named test
  against tests/local-win/posix_shim.h and win_socket_loopback.c, so tests that
  are written against POSIX sockets can be run and debugged on this machine.

  The generated copy differs from the original in two places only, both of
  which have no Windows equivalent; every rewrite is asserted to have applied,
  so a change to the test cannot silently disable one. The original remains the
  single source of truth and is what CI compiles.

  Set WIN_SHIM_TRACE=1 in the environment to trace every socket call.
#>
param(
    [string]$Test = 'test_api_client',
    [string]$WorkDir = (Join-Path $env:TEMP 'one_os_local_win'),
    [switch]$KeepSource
)

$ErrorActionPreference = 'Stop'

$Repo = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$harness = Join-Path $Repo 'tests\local-win'
$es = Join-Path $Repo 'firmware\components\esphome_l2'

$ccCandidates = @(
    (Join-Path (Split-Path -Parent $Repo) '.toolchain\llvm-mingw-20260908-ucrt-x86_64\bin\clang.exe'),
    (Join-Path $Repo '.toolchain\llvm-mingw-20260908-ucrt-x86_64\bin\clang.exe')
)
$cc = $ccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cc) {
    $onPath = Get-Command clang -ErrorAction SilentlyContinue
    if ($onPath) { $cc = $onPath.Source } else { throw 'no clang found; install the pinned toolchain' }
}

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

# A directory holding the header names the test sources include, each
# forwarding to the shim. <lwip/sockets.h> and <lwip/netdb.h> must also exist
# because the firmware sources include those spellings directly.
$fwd = Join-Path $WorkDir 'shiminc'
$fwdLwip = Join-Path $fwd 'lwip'
New-Item -ItemType Directory -Force -Path $fwd, $fwdLwip | Out-Null
Set-Content -LiteralPath (Join-Path $fwd 'sys_socket_forward.h') -Value '#include "posix_shim.h"'
Set-Content -LiteralPath (Join-Path $fwdLwip 'sockets.h') -Value '#include "posix_shim.h"'
Set-Content -LiteralPath (Join-Path $fwdLwip 'netdb.h') -Value '#include "posix_shim.h"'
Set-Content -LiteralPath (Join-Path $fwd 'unistd.h') -Value '#include "posix_shim.h"'
Set-Content -LiteralPath (Join-Path $fwd 'fcntl.h') -Value '#include "posix_shim.h"'

$portable = Join-Path $WorkDir "$Test.win.c"
$text = Get-Content -Raw -LiteralPath (Join-Path $Repo "tests\esphome_l2\$Test.c")

$rewrites = @(
    @{ From = '.sin_addr.s_addr=htonl(INADDR_LOOPBACK)'; To = '.sin_addr=htonl(INADDR_LOOPBACK)' },
    @{ From = 'signal(SIGPIPE,SIG_IGN);'; To = 'signal(SIGINT,SIG_IGN);' },
    @{ From = 'signal(SIGPIPE, SIG_IGN);'; To = 'signal(SIGINT, SIG_IGN);' }
)
$applied = 0
foreach ($r in $rewrites) {
    if ($text.Contains($r.From)) {
        $text = $text.Replace($r.From, $r.To)
        $applied++
    }
}
if ($applied -eq 0) {
    throw "no Windows rewrite applied to $Test.c; the harness would compile something the test does not mean"
}
# <sys/socket.h> and <arpa/inet.h> do not exist on Windows; point both at the shim.
$text = $text.Replace('#include <sys/socket.h>', '#include "posix_shim.h"')
$text = $text.Replace('#include <arpa/inet.h>', '#include "posix_shim.h"')
Set-Content -LiteralPath $portable -Value $text -NoNewline

$exe = Join-Path $WorkDir "$Test.exe"
$args = @(
    '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
    '-include', 'posix_shim.h',
    '-include', 'sys_socket_forward.h',
    "-I$harness",
    "-I$fwd",
    "-I$(Join-Path $Repo 'tests\esphome_l2\stubs')",
    "-I$(Join-Path $Repo 'tests\esphome_l2')",
    "-I$(Join-Path $es 'include')",
    "-I$es",
    (Join-Path $harness 'win_socket_loopback.c'),
    (Join-Path $es 'esphome_api.c'),
    (Join-Path $es 'esphome_api_codec.c'),
    (Join-Path $es 'esphome_noise.c'),
    (Join-Path $es 'esphome_noise_crypto.c'),
    (Join-Path $Repo 'tests\esphome_l2\noise_test_responder.c'),
    $portable,
    '-pthread', '-o', $exe
)

& $cc @args
if ($LASTEXITCODE -ne 0) {
    throw 'compile failed'
}

$env:PATH = "$(Split-Path -Parent $cc);$env:PATH"
& $exe
$code = $LASTEXITCODE

if (-not $KeepSource) {
    Remove-Item -LiteralPath $portable -ErrorAction SilentlyContinue
}
exit $code
