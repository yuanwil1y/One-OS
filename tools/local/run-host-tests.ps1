<#
    Local host-test entry point for One-OS on Windows.

    CI runs tests/run_all_host_tests.sh, which invokes the per-group runners in
    tests/host-test-groups.txt. Those runners are POSIX shell, and a Windows
    machine without a POSIX shell or a C compiler cannot execute them. This
    script therefore performs the SAME compile and run steps as each runner, with
    the SAME flags, sources, include directories and environment, so a local pass
    means what a CI pass means.

    It is a developer convenience only: the manifest, the runners and CI remain
    the source of truth for what the tests are. Nothing in the repository build
    depends on this file.

    Usage:
        powershell -File tools/local/run-host-tests.ps1
        powershell -File tools/local/run-host-tests.ps1 -Group app_device
        powershell -File tools/local/run-host-tests.ps1 -List
#>
[CmdletBinding()]
param(
    [string]$Group = '',
    [switch]$List,
    [string]$Root = '',
    [string]$Toolchain = ''
)

$ErrorActionPreference = 'Stop'

if (-not $Root) {
    $Root = (Resolve-Path (Join-Path $PSScriptRoot '..\..')).Path
}
if (-not $Toolchain) {
    $Toolchain = $env:DSH_LOCAL_TOOLCHAIN
}
if (-not $Toolchain) {
    $found = Get-ChildItem 'D:\OS\.toolchain' -Directory -ErrorAction SilentlyContinue |
        Where-Object { Test-Path (Join-Path $_.FullName 'bin\clang.exe') } |
        Select-Object -First 1
    if ($found) { $Toolchain = $found.FullName }
}
if (-not $Toolchain) {
    throw 'no C compiler configured. Set DSH_LOCAL_TOOLCHAIN to an llvm-mingw root.'
}

$env:PATH = (Join-Path $Toolchain 'bin') + ';' + $env:PATH
$CC = (Get-Command clang.exe -ErrorAction Stop).Source
$env:CC = 'clang'

$manifest = Join-Path $Root 'tests\host-test-groups.txt'
if (-not (Test-Path $manifest)) { throw "missing group manifest: $manifest" }

$work = Join-Path $env:TEMP 'one_os_local_host_tests'
New-Item -ItemType Directory -Force -Path $work | Out-Null

$sanitize = @('-fsanitize=address,undefined', '-fno-omit-frame-pointer')
$common = @('-std=c11', '-O1', '-g', '-Wall', '-Wextra', '-Werror', '-pedantic')
$env:ASAN_OPTIONS = 'detect_leaks=0'

function Invoke-Step {
    param([string]$Label, [string]$Exe, [string[]]$Arguments)
    Write-Host "    $Label"
    & $Exe @Arguments
    if ($LASTEXITCODE -ne 0) {
        throw "step failed with exit $LASTEXITCODE ($Label)"
    }
}

function Build-And-Run {
    param(
        [string]$Name,
        [string[]]$Sources,
        [string[]]$Includes,
        [string[]]$Extra = @()
    )
    $exe = Join-Path $work "$Name.exe"
    if (Test-Path $exe) { Remove-Item $exe -Force }
    $compileArgs = @($common) + @($sanitize) + @($Extra) + @($Includes) + @($Sources) + @('-o', $exe)
    Invoke-Step 'compile' $CC $compileArgs
    Invoke-Step 'run' $exe @()
}

function Get-Groups {
    Get-Content $manifest | ForEach-Object {
        $line = $_.Trim()
        if ($line -eq '' -or $line.StartsWith('#')) { return }
        $parts = $line -split '\s+', 2
        [pscustomobject]@{ Name = $parts[0]; Runner = $parts[1] }
    }
}

function Invoke-Group {
    param([string]$Name)

    $m = Join-Path $Root 'firmware\main'
    $hc = Join-Path $Root 'firmware\components\ha_core'
    $stubs = Join-Path $Root 'tests\host\stubs'
    $py = (Get-Command python.exe -ErrorAction Stop).Source

    switch ($Name) {
        'ha_l2' {
            $exe = Join-Path $work 'ha_l2.exe'
            Invoke-Step 'compile' $CC (@($common) + @($sanitize) + @(
                    "-I$(Join-Path $hc 'include')",
                    "-I$(Join-Path $Root 'firmware\components\ha_discovery_l2\include')",
                    "-I$(Join-Path $Root 'firmware\components\ha_discovery_l2')",
                    (Join-Path $Root 'tests\ha_l2_host_tests.c'),
                    (Join-Path $hc 'ha_core.c'),
                    (Join-Path $Root 'firmware\components\ha_discovery_l2\ha_mdns_parser.c'),
                    (Join-Path $Root 'firmware\components\ha_discovery_l2\ha_ssdp_parser.c'),
                    '-o', $exe))
            Invoke-Step 'run' $exe @()
        }
        'esphome_l2' {
            $inc = @(
                "-I$(Join-Path $Root 'tests\esphome_l2\stubs')",
                "-I$(Join-Path $Root 'firmware\components\esphome_l2\include')",
                "-I$(Join-Path $Root 'firmware\components\esphome_l2')")
            $flags = @('-std=gnu11', '-Wall', '-Wextra', '-Werror', '-O2')
            $es = Join-Path $Root 'firmware\components\esphome_l2'

            $g = Join-Path $work 'test_gatt.exe'
            Invoke-Step 'compile test_gatt' $CC (@($flags) + @($inc) + @(
                    (Join-Path $es 'esphome_ble_gatt.c'),
                    (Join-Path $Root 'tests\esphome_l2\test_gatt.c'), '-o', $g))
            Invoke-Step 'run test_gatt' $g @()

            $c = Join-Path $work 'test_codec.exe'
            Invoke-Step 'compile test_codec' $CC (@($flags) + @($inc) + @(
                    (Join-Path $es 'esphome_api_codec.c'),
                    (Join-Path $Root 'tests\esphome_l2\test_codec.c'), '-lm', '-o', $c))
            Invoke-Step 'run test_codec' $c @()

            $a = Join-Path $work 'test_api_client.exe'
            Invoke-Step 'compile test_api_client' $CC (@($flags) + @($inc) + @(
                    (Join-Path $es 'esphome_api.c'),
                    (Join-Path $es 'esphome_api_codec.c'),
                    (Join-Path $Root 'tests\esphome_l2\test_api_client.c'),
                    '-pthread', '-o', $a))
            Invoke-Step 'run test_api_client' $a @()
        }
        'theengs_l2' {
            $te = Join-Path $Root 'firmware\components\theengs_l2'
            $exe = Join-Path $work 'theengs_l2.exe'
            Invoke-Step 'compile' $CC (@($common) + @(
                    "-I$(Join-Path $te 'include')",
                    (Join-Path $te 'theengs_l2.c'),
                    (Join-Path $te 'theengs_ruuvi.c'),
                    (Join-Path $te 'theengs_bthome.c'),
                    (Join-Path $te 'tests\test_theengs_l2.c'),
                    '-lm', '-o', $exe))
            Invoke-Step 'run' $exe @()
        }
        'zha_zigpy_l2' {
            $flags = @('-std=c11', '-Wall', '-Wextra', '-Werror', '-pedantic')
            $z1 = Join-Path $work 'test_zigpy_l2.exe'
            Invoke-Step 'compile test_zigpy_l2' $CC (@($flags) + @(
                    "-I$(Join-Path $Root 'firmware\components\zigpy_l2\include')",
                    (Join-Path $Root 'firmware\components\zigpy_l2\zigpy_l2.c'),
                    (Join-Path $Root 'tests\host\test_zigpy_l2.c'), '-o', $z1))
            Invoke-Step 'run test_zigpy_l2' $z1 @()

            $z2 = Join-Path $work 'test_zha_l2.exe'
            Invoke-Step 'compile test_zha_l2' $CC (@($flags) + @(
                    "-I$(Join-Path $Root 'firmware\components\zha_l2\include')",
                    (Join-Path $Root 'firmware\components\zha_l2\zha_l2.c'),
                    (Join-Path $Root 'tests\host\test_zha_l2.c'), '-o', $z2))
            Invoke-Step 'run test_zha_l2' $z2 @()

            $hit = Get-ChildItem (Join-Path $Root 'firmware\components\zigpy_l2') -Recurse -File |
                Select-String -Pattern '#include\s+[<"]zha_l2|\bzha_[A-Za-z0-9_]*'
            if ($hit) { throw "zigpy_l2 must not depend on zha_l2: $($hit[0])" }
            $hit = Get-ChildItem (Join-Path $Root 'firmware\components\zha_l2') -Recurse -File |
                Select-String -Pattern '#include\s+[<"]zigpy_l2|\bzigpy_[A-Za-z0-9_]*'
            if ($hit) { throw "zha_l2 must not depend on zigpy_l2: $($hit[0])" }
            Write-Host '    family-boundary checks passed'
        }
        'openthread_l2' {
            $ot = Join-Path $Root 'firmware\components\openthread_l2'
            $exe = Join-Path $work 'openthread_l2.exe'
            Invoke-Step 'compile' $CC (@('-std=c11', '-Wall', '-Wextra', '-Werror') + @(
                    "-I$(Join-Path $ot 'include')", "-I$ot",
                    (Join-Path $ot 'openthread_l2_util.c'),
                    (Join-Path $Root 'tests\openthread_l2_host_test.c'), '-o', $exe))
            Invoke-Step 'run' $exe @()
        }
        'kismet_l2' {
            $k = Join-Path $Root 'firmware\components\kismet_l2'
            $exe = Join-Path $work 'kismet_l2.exe'
            Invoke-Step 'compile' $CC (@('-std=c11', '-Wall', '-Wextra', '-Werror') + @(
                    "-I$(Join-Path $Root 'tools\kismet_l2_host_tests\stub')",
                    "-I$(Join-Path $k 'include')", "-I$k",
                    (Join-Path $Root 'tools\kismet_l2_host_tests\kismet_l2_host_test.c'),
                    (Join-Path $k 'kismet_wifi_tracker.c'),
                    (Join-Path $k 'kismet_wifi_decode.c'),
                    (Join-Path $k 'kismet_ble_tracker.c'), '-o', $exe))
            Invoke-Step 'run' $exe @()
        }
        'nmap_l2' {
            $n = Join-Path $Root 'firmware\components\nmap_l2'
            $exe = Join-Path $work 'nmap_l2.exe'
            Invoke-Step 'compile' $CC (@('-std=c11', '-Wall', '-Wextra', '-Werror') + @(
                    "-I$stubs", "-I$(Join-Path $n 'include')", "-I$n",
                    (Join-Path $n 'nmap_core.c'),
                    (Join-Path $n 'nmap_service.c'),
                    (Join-Path $Root 'tests\host\test_nmap_l2.c'), '-o', $exe))
            Invoke-Step 'run' $exe @()
        }
        'wireshark_l2' {
            $ws = Join-Path $Root 'tests\host\wireshark_l2'
            $exe = Join-Path $work 'wireshark_l2.exe'
            Invoke-Step 'compile' $CC (@('-std=c11', '-Wall', '-Wextra', '-Werror', '-O1', '-g',
                    '-fsanitize=address,undefined') + @(
                    "-I$(Join-Path $Root 'firmware\components\wireshark_l2\include')",
                    (Join-Path $Root 'firmware\components\wireshark_l2\wireshark_wifi.c'),
                    (Join-Path $Root 'firmware\components\wireshark_l2\wireshark_ble.c'),
                    (Join-Path $ws 'test_wireshark_l2.c'), '-o', $exe))
            Invoke-Step 'run' $exe @()
        }
        'app_diag_protocol' {
            Build-And-Run 'app_diag_protocol' @(
                (Join-Path $m 'app_diag_protocol.c'),
                (Join-Path $Root 'tests\host\test_app_diag_protocol.c')
            ) @("-I$stubs", "-I$(Join-Path $m 'include')")
        }
        'app_ops' {
            Build-And-Run 'app_ops' @(
                (Join-Path $m 'app_ops.c'),
                (Join-Path $Root 'tests\host\test_app_ops.c')
            ) @("-I$stubs", "-I$(Join-Path $m 'include')")
        }
        'app_scan' {
            Build-And-Run 'app_scan' @(
                (Join-Path $m 'app_str.c'),
                (Join-Path $m 'app_ops.c'),
                (Join-Path $m 'app_scan.c'),
                (Join-Path $Root 'tests\host\test_app_scan.c')
            ) @("-I$stubs", "-I$(Join-Path $m 'include')")
        }
        'app_device' {
            Build-And-Run 'app_device' @(
                (Join-Path $hc 'ha_core.c'),
                (Join-Path $m 'app_str.c'),
                (Join-Path $m 'app_ops.c'),
                (Join-Path $m 'app_scan.c'),
                (Join-Path $m 'app_recognition.c'),
                (Join-Path $m 'app_device.c'),
                (Join-Path $Root 'tests\host\stubs\app_l2_lookup_stub.c'),
                (Join-Path $Root 'tests\host\test_app_device.c')
            ) @("-I$stubs", "-I$(Join-Path $m 'include')", "-I$(Join-Path $hc 'include')")
        }
        'app_device_db' {
            # No -D: the group resolves the fixture path from its own __FILE__ when
            # DEVICE_DB_FIXTURE_DIR is not defined, which avoids the
            # quoting-a-Windows-path problem entirely.
            Build-And-Run 'app_device_db' @(
                (Join-Path $hc 'ha_core.c'),
                (Join-Path $m 'app_str.c'),
                (Join-Path $m 'app_ops.c'),
                (Join-Path $m 'app_scan.c'),
                (Join-Path $m 'device_db_format.c'),
                (Join-Path $m 'app_recognition.c'),
                (Join-Path $m 'app_device_db.c'),
                (Join-Path $m 'app_device.c'),
                (Join-Path $Root 'tests\host\stubs\app_l2_lookup_stub.c'),
                (Join-Path $Root 'tests\host\test_app_device_db.c')
            ) @("-I$stubs", "-I$(Join-Path $m 'include')", "-I$(Join-Path $hc 'include')")
        }
        'app_db_import' {
            # app_recognition.c is here for app_db_state_name(); it needs the two L2
            # lookup doubles, exactly as the app_device group does.
            # No -D: the group resolves the fixture path from its own __FILE__ when
            # DEVICE_DB_FIXTURE_DIR is not defined. CI passes it explicitly.
            Build-And-Run 'app_db_import' @(
                (Join-Path $m 'app_str.c'),
                (Join-Path $m 'device_db_format.c'),
                (Join-Path $m 'app_recognition.c'),
                (Join-Path $m 'app_db_import.c'),
                (Join-Path $Root 'tests\host\stubs\app_l2_lookup_stub.c'),
                (Join-Path $Root 'tests\host\test_app_db_import.c')
            ) @("-I$stubs", "-I$(Join-Path $m 'include')", "-I$(Join-Path $hc 'include')")
        }
        'app_portal' {
            Build-And-Run 'app_portal' @(
                (Join-Path $m 'app_str.c'),
                (Join-Path $m 'app_portal.c'),
                (Join-Path $Root 'tests\host\test_app_portal.c')
            ) @("-I$stubs", "-I$(Join-Path $m 'include')", "-I$(Join-Path $hc 'include')")
        }
        'device_db_python' {
            Invoke-Step 'regenerate fixture' $py @((Join-Path $Root 'tools\device_db\build_device_db.py'))
            Invoke-Step 'regenerate invalid variants' $py @((Join-Path $Root 'tools\device_db\make_invalid_fixtures.py'))
            Invoke-Step 'python tests' $py @((Join-Path $Root 'tools\device_db\test_device_db_python.py'))
        }
        'device_db_format' {
            Invoke-Step 'regenerate fixture' $py @((Join-Path $Root 'tools\device_db\build_device_db.py'))
            Invoke-Step 'regenerate invalid variants' $py @((Join-Path $Root 'tools\device_db\make_invalid_fixtures.py'))
            Invoke-Step 'reproducibility check' $py @((Join-Path $Root 'tools\device_db\build_device_db.py'), '--check')
            $fixtureDir = (Join-Path $Root 'tests\fixtures\device_db') -replace '\\', '/'
            Invoke-Step 'python validator accepts fixture' $py @(
                (Join-Path $Root 'tools\device_db\validate_device_db.py'),
                (Join-Path $fixtureDir 'devices_fixture.nbdb'))
            $rejected = 0
            foreach ($variant in (Get-Content (Join-Path $fixtureDir 'invalid\manifest.txt'))) {
                $v = $variant.Trim()
                if ($v -eq '') { continue }
                & $py (Join-Path $Root 'tools\device_db\validate_device_db.py') `
                    (Join-Path $fixtureDir "invalid/$v") --expect-failure *> $null
                if ($LASTEXITCODE -ne 0) { throw "validator accepted damaged variant $v" }
                $rejected++
            }
            Write-Host "    python rejected $rejected variants"
            # No -D: the group resolves the fixture path from __FILE__ when
            # DEVICE_DB_FIXTURE_DIR is not defined, which avoids quoting a Windows
            # path through a -D flag. CI still passes -D explicitly.
            Build-And-Run 'device_db_format' @(
                (Join-Path $m 'device_db_format.c'),
                (Join-Path $Root 'tests\host\test_device_db_format.c')
            ) @("-I$(Join-Path $m 'include')")
        }
        default {
            throw "group '$Name' has no local equivalent; add one to tools/local/run-host-tests.ps1"
        }
    }
}

$groups = @(Get-Groups)
if ($List) {
    $groups | ForEach-Object { $_.Name }
    return
}
if ($Group) {
    $groups = @($groups | Where-Object { $_.Name -eq $Group })
    if ($groups.Count -eq 0) { throw "no such group: $Group" }
}

$passed = 0
$failed = 0
$failedNames = @()
foreach ($g in $groups) {
    Write-Host ''
    Write-Host "=== host test group: $($g.Name) ==="
    try {
        Invoke-Group $g.Name
        Write-Host "--- $($g.Name): PASS"
        $passed++
    } catch {
        Write-Host "--- $($g.Name): FAIL"
        Write-Host "    $($_.Exception.Message)"
        $failed++
        $failedNames += $g.Name
    }
}

Write-Host ''
Write-Host '================ host test summary ================'
Write-Host "passed groups: $passed"
Write-Host "failed groups: $failed"
if ($failed -gt 0) {
    Write-Host "failed: $($failedNames -join ' ')"
    exit 1
}
Write-Host 'all host test groups passed'
