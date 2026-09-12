<#
.SYNOPSIS
  Run a socket-based host test on Windows, on the in-process loopback.

.DESCRIPTION
  tests/esphome_l2/test_api_client.c is written against POSIX sockets, which
  llvm-mingw does not provide, so this machine could not compile it and every
  defect in it showed up only in CI. tests/esphome_l2/stubs/win supplies an
  in-process loopback, and this script builds the test against it so the same
  test runs here.

  Two constructs have no Windows equivalent and are rewritten in a copy of the
  test source generated into the build directory: the nested struct designator
  for an IPv4 address, and SIGPIPE (Windows has no such signal; the loopback
  reports a closed peer as EPIPE instead). Every rewrite is asserted to have
  applied, so a change to the test cannot silently disable one. The POSIX
  original remains the single source of truth and is what CI compiles.

  This is a debugging aid for this machine, not a replacement for CI: the
  loopback is not a TCP stack and a test that passes here still has to pass
  there.
#>
param(
    [string]$WorkDir = (Join-Path $env:TEMP 'one_os_win_shim'),
    [switch]$KeepOutput
)

$ErrorActionPreference = 'Stop'

$Root = Split-Path -Parent (Split-Path -Parent $PSScriptRoot)
$Repo = $Root
$shim = Join-Path $Repo 'tests\esphome_l2\stubs\win'
$es = Join-Path $Repo 'firmware\components\esphome_l2'

# The toolchain lives beside the checkout, not inside it.
$ccCandidates = @(
    (Join-Path (Split-Path -Parent $Repo) '.toolchain\llvm-mingw-20260908-ucrt-x86_64\bin\clang.exe'),
    (Join-Path $Repo '.toolchain\llvm-mingw-20260908-ucrt-x86_64\bin\clang.exe')
)
$cc = $ccCandidates | Where-Object { Test-Path $_ } | Select-Object -First 1
if (-not $cc) {
    $onPath = Get-Command clang -ErrorAction SilentlyContinue
    if ($onPath) { $cc = $onPath.Source } else { throw 'no clang found; pass one or install the pinned toolchain' }
}

New-Item -ItemType Directory -Force -Path $WorkDir | Out-Null

$portable = Join-Path $WorkDir 'win_portable_test.c'
$text = Get-Content -Raw -LiteralPath (Join-Path $Repo 'tests\esphome_l2\test_api_client.c')

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
    throw "no Windows rewrite applied; the shim would compile something the test does not mean"
}
Set-Content -LiteralPath $portable -Value $text -NoNewline

$exe = Join-Path $WorkDir 'test_api_client.exe'
$args = @(
    '-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
    '-include', 'sys/socket.h', '-include', 'lwip/sockets.h', '-include', 'lwip/netdb.h',
    '-Dsuseconds_t=useconds_t',
    "-I$shim",
    "-I$(Join-Path $Repo 'tests\esphome_l2\stubs')",
    "-I$(Join-Path $Repo 'tests\esphome_l2')",
    "-I$(Join-Path $es 'include')",
    "-I$es",
    (Join-Path $shim 'win_socket_loopback.c'),
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
    throw "compile failed"
}

# The console tests need the toolchain's DLLs on PATH.
$env:PATH = "$(Split-Path -Parent $cc);$env:PATH"
$env:WIN_SHIM_TRACE = if ($env:WIN_SHIM_TRACE) { $env:WIN_SHIM_TRACE } else { '0' }
& $exe
$code = $LASTEXITCODE

if (-not $KeepOutput) {
    Remove-Item -LiteralPath $portable -ErrorAction SilentlyContinue
}
exit $code
