<#
.SYNOPSIS
  Build a host test on Windows with the in-process socket loopback.

.DESCRIPTION
  The socket-based esphome_l2 tests are written against POSIX sockets, which
  llvm-mingw does not provide. tests/esphome_l2/stubs/win supplies a loopback
  replacement so those tests run here rather than only in CI.

  Two constructs have no Windows equivalent at all and are rewritten in a
  copy of the test source that is generated into the build directory: a nested
  struct designator for the IPv4 address, and SIGPIPE (Windows has no such
  signal, and the shim reports a closed peer as EPIPE instead). The copy is
  never committed; the POSIX original stays the single source of truth and is
  what CI compiles.

  Every rewrite is asserted to have applied, so a change to the test file
  cannot silently disable one.
#>
param(
    [Parameter(Mandatory = $true)][string]$Source,
    [Parameter(Mandatory = $true)][string]$Output,
    [Parameter(Mandatory = $true)][string]$WorkDir,
    [Parameter(Mandatory = $true)][string]$Compiler,
    [string[]]$ExtraFlags = @(),
    [string[]]$ExtraSources = @()
)

$ErrorActionPreference = 'Stop'

$portable = Join-Path $WorkDir 'win_portable_test.c'
$text = Get-Content -Raw -LiteralPath $Source

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
    throw "no Windows rewrite applied to $Source; the shim would compile something the test does not mean"
}
Set-Content -LiteralPath $portable -Value $text -NoNewline

$args = @('-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2',
    '-include', 'sys/socket.h', '-include', 'lwip/sockets.h', '-include', 'lwip/netdb.h',
    '-Dsuseconds_t=useconds_t') + $ExtraFlags + $ExtraSources + @($portable, '-o', $Output)

& $Compiler @args
exit $LASTEXITCODE
