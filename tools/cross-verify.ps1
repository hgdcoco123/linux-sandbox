<#
.SYNOPSIS
    Verify this tree on Windows: portable suites, Linux cross-compile, Linux link.

.DESCRIPTION
    There is no Linux host available during development, so this script is the
    strongest check that can run locally:

      1. build and run every portable test suite with the native compiler
      2. cross-compile every Linux translation unit with zig
      3. link sandbox and sandbox-launcher for x86_64-linux

    Step 3 matters: compiling each file separately still allows a missing or
    duplicated symbol to survive. Linking is what proves the tree is coherent.

    It does NOT prove runtime behaviour. Namespaces, cgroup, seccomp, signal
    ordering and cleanup deadlines still have to be validated on a real kernel.

.PARAMETER Zig
    Path to zig.exe (0.13 tested).

.PARAMETER HostWork
    Directory holding the Linux sysroot and the prebuilt protobuf objects
    (usr/include + usr/lib/x86_64-linux-gnu/libseccomp.a, libpb_*.o and
    session_v1.pb.cc.o). Cross-linking cannot work without it.

.EXAMPLE
    pwsh tools/cross-verify.ps1 -Zig C:\zig\zig.exe -HostWork C:\work
#>
param(
    [Parameter(Mandatory = $true)][string]$Zig,
    [Parameter(Mandatory = $true)][string]$HostWork,
    [string]$Build = "$PSScriptRoot\..\build-cross",
    [string]$Target = "x86_64-linux-gnu.2.38"
)

$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
New-Item -ItemType Directory -Force $Build | Out-Null
$env:ZIG_GLOBAL_CACHE_DIR = Join-Path $Build 'zig-global'
$env:ZIG_LOCAL_CACHE_DIR = Join-Path $Build 'zig-local'
$failures = 0

function Step([string]$name) { Write-Host "== $name" -ForegroundColor Cyan }

# 1. Portable suites. These are the only checks that actually execute code.
Step 'portable suites'
$core = Get-ChildItem (Join-Path $root 'src/core') -Filter *.cpp | ForEach-Object { $_.FullName }
$suites = [ordered]@{
    core       = 'tests/core_tests.cpp'
    wire       = 'tests/wire_tests.cpp'
    supervisor = 'tests/supervisor_tests.cpp'
    prepare    = 'tests/prepare_tests.cpp'
    control    = 'tests/control_tests.cpp'
    workspace  = 'tests/workspace_tests.cpp'
    fault      = 'tests/fault_tests.cpp'
    model      = 'tests/model_tests.cpp'
}
foreach ($name in $suites.Keys) {
    $extra = if ($name -eq 'model') {
        @('src/model/config.cpp', 'src/model/report.cpp', 'src/model/command_model.cpp',
          'src/model/task.cpp', 'src/core/workspace.cpp') |
            ForEach-Object { Join-Path $root $_ }
    }
    elseif ($name -in 'wire', 'control', 'workspace') {
        $core | Where-Object { $_ -notmatch 'supervisor' }
    }
    else { $core }
    $exe = Join-Path $Build "$name-tests.exe"
    & g++ -std=c++20 -O1 -Wall -Wextra -Wno-unused-result -Wno-maybe-uninitialized `
        -I (Join-Path $root 'include') -I (Join-Path $root 'third_party') `
        -o $exe @extra (Join-Path $root $suites[$name])
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL build $name" -ForegroundColor Red; $failures++; continue }
    & $exe
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL run $name" -ForegroundColor Red; $failures++ }
}

# 1.4 Shipped profiles must still parse under the current config schema.
Step 'profiles'
$modelExe = Join-Path $Build 'model-tests.exe'
if (Test-Path $modelExe) {
    & $modelExe (Join-Path $root 'profiles')
    if ($LASTEXITCODE -ne 0) { Write-Host 'FAIL profile schema check' -ForegroundColor Red; $failures++ }
}
# 1.5 CMakeLists must list exactly the sources that exist. This script compiles
# the tree directly, so without this check a file missing from CMake would build
# here and fail on a real Linux CMake build.
Step 'cmake source list'
$cmake = Get-Content (Join-Path $root 'CMakeLists.txt') -Raw
$listed = [regex]::Matches($cmake, '(?m)(?:^|\s)((?:src|apps)/[A-Za-z0-9_/]+\.cpp)') |
    ForEach-Object { $_.Groups[1].Value } | Sort-Object -Unique
$present = @(Get-ChildItem (Join-Path $root 'src') -Recurse -Filter *.cpp) +
           @(Get-ChildItem (Join-Path $root 'apps') -Recurse -Filter *.cpp) | ForEach-Object {
    $_.FullName.Substring($root.Length + 1) -replace '\\', '/'
} | Sort-Object -Unique
$drifted = @($present | Where-Object { $_ -notin $listed }) +
           @($listed | Where-Object { $_ -notin $present })
if ($drifted.Count -ne 0) {
    Write-Host "FAIL cmake source list drift: $($drifted -join ', ')" -ForegroundColor Red
    $failures++
}
# 2. Cross-compile every Linux translation unit.
Step 'linux cross-compile'
$includes = @(
    "-I$(Join-Path $root 'include')",
    "-I$(Join-Path $root 'third_party')",
    "-I$(Join-Path $root 'third_party/spdlog/include')",
    "-I$(Join-Path $HostWork 'sysroot/usr/include')",
    "-I$(Join-Path $HostWork 'protobuf-cross-build')",
    "-I$(Join-Path $HostWork 'protobuf-source/protobuf-3.21.12/src')"
)
$objects = @()
foreach ($source in @(Get-ChildItem (Join-Path $root 'src') -Recurse -Filter *.cpp)) {
    $name = ($source.FullName.Substring($root.Length + 1) -replace '[\\/]', '_')
    $object = Join-Path $Build "$name.o"
    & $Zig c++ -target $Target -std=c++20 -O1 -c @includes -o $object $source.FullName
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL compile $name" -ForegroundColor Red; $failures++ }
    else { $objects += $object }
}
# The two executables have their own main(); compile them but keep them out of
# the shared object list so the link step cannot pick up both.
foreach ($source in @(Get-ChildItem (Join-Path $root 'apps') -Recurse -Filter *.cpp)) {
    $name = ($source.FullName.Substring($root.Length + 1) -replace '[\\/]', '_')
    $object = Join-Path $Build "$name.o"
    & $Zig c++ -target $Target -std=c++20 -O1 -c @includes -o $object $source.FullName
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL compile $name" -ForegroundColor Red; $failures++ }
}

# 2.5 Every Linux test must compile too: CMake builds them, so a break here
# fails a real build even though this script never runs them.
Step 'linux tests compile'
foreach ($source in @(Get-ChildItem (Join-Path $root 'tests') -Filter *.cpp)) {
    $object = Join-Path $Build "$($source.BaseName)-test.o"
    & $Zig c++ -target $Target -std=c++20 -O1 -c @includes -o $object $source.FullName
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL test-compile $($source.Name)" -ForegroundColor Red; $failures++ }
}
# 3. Link. A missing or duplicated symbol only shows up here.
Step 'linux link'
$protobuf = Get-ChildItem (Join-Path $HostWork 'protobuf-cross-build') -Filter 'libpb_*.o' |
    ForEach-Object { $_.FullName }
$generated = Join-Path $HostWork 'protobuf-cross-build/session_v1.pb.cc.o'
$seccomp = Join-Path $HostWork 'sysroot/usr/lib/x86_64-linux-gnu'
foreach ($binary in @(
        @{name = 'sandbox'; main = "$Build/apps_cli_main.cpp.o"},
        @{name = 'sandbox-launcher'; main = "$Build/apps_launcher_main.cpp.o"})) {
    & $Zig c++ -target $Target -o (Join-Path $Build $binary.name) $objects $binary.main $protobuf `
        $generated -L $seccomp -lseccomp -lpthread
    if ($LASTEXITCODE -ne 0) { Write-Host "FAIL link $($binary.name)" -ForegroundColor Red; $failures++ }
}

Step 'summary'
if ($failures -ne 0) {
    Write-Host "$failures failure(s)" -ForegroundColor Red
    exit 1
}
Write-Host 'portable suites green; every Linux TU compiled and both binaries linked' -ForegroundColor Green
Write-Host 'runtime behaviour is still unverified: this needs a real Linux kernel'
