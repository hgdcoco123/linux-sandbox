<#
.SYNOPSIS
    Build and run every portable test suite on Windows.

.DESCRIPTION
    This is the quick smoke test: no cmake, no Linux, no protobuf. It uses the
    system g++ and needs nothing beyond this repository.

    It proves the parts that are pure logic: typed model, result split, wire
    codec, execution core, preparation rollback, control messages, workspace
    boundary, fault boundaries and the config model.

    It proves nothing about Linux behaviour. Use tools/ubuntu-accept.sh for that.

.EXAMPLE
    powershell -ExecutionPolicy Bypass -File tools/portable-tests.ps1
#>
param(
    [string]$Cxx = "g++",
    [string]$Build = "$PSScriptRoot\..\build-portable"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force $Build | Out-Null
$failures = 0
$include = @("-I$(Join-Path $root 'include')", "-I$(Join-Path $root 'third_party')")
$core = Get-ChildItem (Join-Path $root 'src/core') -Filter *.cpp | ForEach-Object { $_.FullName }

$suites = [ordered]@{
    core       = @('tests/core_tests.cpp')
    wire       = @('tests/wire_tests.cpp')
    supervisor = @('tests/supervisor_tests.cpp')
    prepare    = @('tests/prepare_tests.cpp')
    control    = @('tests/control_tests.cpp')
    workspace  = @('tests/workspace_tests.cpp')
    ops_json   = @('tests/ops_json_tests.cpp')
    fault      = @('tests/fault_tests.cpp')
    model      = @('tests/model_tests.cpp',
                   'src/model/config.cpp', 'src/model/report.cpp',
                   'src/model/command_model.cpp', 'src/model/task.cpp')
}

foreach ($name in $suites.Keys) {
    $extra = $suites[$name]
    # supervisor.cpp defines the core the supervisor and fault suites exercise;
    # the codec suites do not need it.
    $sources = if ($name -in 'core', 'supervisor', 'prepare', 'fault') { $core } else {
        $core | Where-Object { $_ -notmatch 'supervisor' }
    }
    $exe = Join-Path $Build "$name-tests.exe"
    Write-Host "== $name" -ForegroundColor Cyan
    & $Cxx -std=c++20 -O1 -Wall -Wextra -Wno-unused-result -Wno-maybe-uninitialized `
        @include -o $exe @sources @($extra | ForEach-Object { Join-Path $root $_ })
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL build $name" -ForegroundColor Red
        $failures++
        continue
    }
    & $exe
    if ($LASTEXITCODE -ne 0) {
        Write-Host "FAIL run $name" -ForegroundColor Red
        $failures++
    }
}

Write-Host ''
if ($failures -ne 0) {
    Write-Host "$failures suite(s) failed" -ForegroundColor Red
    exit 1
}
Write-Host 'all portable suites passed - this says nothing about Linux behaviour' -ForegroundColor Green
