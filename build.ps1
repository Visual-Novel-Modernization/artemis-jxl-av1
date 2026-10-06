<#
.SYNOPSIS
    Build artemis_jxl.dll (JXL hook + AV1 MFT) and launcher.exe.
.DESCRIPTION
    Needs an MSYS2 mingw32 environment with the i686 gcc, libjxl, libpng
    and dav1d packages installed.
.PARAMETER Target
    test, dll, launcher, all (default) or clean.
.PARAMETER Msys
    MSYS2 root; auto-detected when omitted.
.EXAMPLE
    .\build.ps1 all
#>
[CmdletBinding()]
param(
    [ValidateSet('test', 'dll', 'launcher', 'all', 'clean')]
    [string]$Target = 'all',

    [string]$Msys = ''
)

$ErrorActionPreference = 'Stop'

$RepoRoot = $PSScriptRoot
$SrcDir   = Join-Path $RepoRoot 'src'
$OutDir   = Join-Path $RepoRoot 'build'

function Write-Step { param([string]$m) Write-Host "`n=== $m ===" -ForegroundColor Cyan }
function Write-Ok   { param([string]$m) Write-Host "  OK   $m"   -ForegroundColor Green }
function Write-Warn2{ param([string]$m) Write-Host "  WARN $m"   -ForegroundColor Yellow }
function Write-Err  { param([string]$m) Write-Host "  FAIL $m"   -ForegroundColor Red }

function Resolve-Msys {
    # standard MSYS2 install spots, not developer paths
    $candidates = @(
        'C:\msys64'
        'C:\msys32'
        'C:\tools\msys64'
        (Join-Path $env:USERPROFILE 'scoop\apps\msys2\current')
        (Join-Path $env:USERPROFILE 'scoop\apps\msys2\current\msys64')
    )
    foreach ($p in $candidates) {
        if ($p -and (Test-Path (Join-Path $p 'mingw32\bin\g++.exe'))) { return $p }
    }

    $g = Get-Command g++ -ErrorAction SilentlyContinue
    if ($g) {
        $binDir = Split-Path $g.Source -Parent
        $root   = Split-Path (Split-Path $binDir -Parent) -Parent
        if (Test-Path (Join-Path $root 'mingw32\include')) { return $root }
        return (Split-Path $binDir -Parent)
    }
    return $null
}

if (-not $Msys) {
    $Msys = Resolve-Msys
    if (-not $Msys) {
        throw "Could not find an MSYS2 (mingw32) environment. Install MSYS2 and run:`n`n" +
              "    pacman -S mingw-w64-i686-gcc mingw-w64-i686-libjxl " +
              "mingw-w64-i686-libpng mingw-w64-i686-dav1d`n`n" +
              "If MSYS2 is not in a default location, pass it explicitly:`n`n    .\build.ps1 -Msys '<MSYS2 root>'"
    }
}

$Mingw  = Join-Path $Msys  'mingw32'
$Gxx    = Join-Path $Mingw 'bin\g++.exe'
$IncDir = Join-Path $Mingw 'include'
$LibDir = Join-Path $Mingw 'lib'

Write-Verbose "MSYS2 root : $Msys"
Write-Verbose "g++        : $Gxx"

# static link order matters: libjxl pulls in hwy and brotli
$JxlLibs = @(
    'libjxl.a'
    'libjxl_threads.a'
    'libhwy.a'
    'libbrotlidec.a'
    'libbrotlicommon.a'
    'libbrotlienc.a'
) | ForEach-Object { Join-Path $LibDir $_ }

$PngLibs = @(
    (Join-Path $LibDir 'libpng16.a')
    (Join-Path $LibDir 'libz.a')
)

$Dav1dLibs = @( (Join-Path $LibDir 'libdav1d.a') )

# no -m32: mingw32 already targets i686 and GCC rejects the flag.
# without *_STATIC_DEFINE the libjxl headers expand to dllimport and linking fails
$CommonFlags = @(
    '-O2', '-Wall'
    "-I$IncDir", "-I$SrcDir"
    '-DWIN32_LEAN_AND_MEAN'
    '-DJXL_STATIC_DEFINE', '-DHWY_STATIC_DEFINE', '-DBROTLI_STATIC_DEFINE'
)

$StaticFlags = @(
    '-static', '-static-libgcc', '-static-libstdc++'
    '-Wl,--exclude-libs,ALL'
)

function Invoke-Tool {
    param(
        [Parameter(Mandatory)][string]   $Exe,
        [Parameter(Mandatory)][string[]] $ArgList,
        [string] $Label = ''
    )
    Write-Verbose ("{0} {1}" -f $Exe, ($ArgList -join ' '))
    $output = & $Exe @ArgList 2>&1
    $code   = $LASTEXITCODE
    if ($output) { foreach ($line in $output) { Write-Host "    $line" } }
    if ($code -ne 0 -and $Label) { Write-Err "$Label (exit=$code)" }
    return $code
}

function Test-Prereq {
    $missing = @()
    foreach ($p in @($Gxx) + $JxlLibs + $PngLibs + $Dav1dLibs) {
        if (-not (Test-Path $p)) { $missing += $p }
    }
    if ($missing.Count) {
        Write-Err 'Missing the following dependencies:'
        $missing | ForEach-Object { Write-Host "        $_" -ForegroundColor Red }
        throw 'Toolchain or static libraries are incomplete. Make sure mingw-w64-i686-gcc / -libjxl / -libpng / -dav1d are installed'
    }
    $mingwBin = Join-Path $Mingw 'bin'
    if (($env:PATH -split ';') -notcontains $mingwBin) {
        $env:PATH = "$mingwBin;$env:PATH"
    }
    if (-not (Test-Path $Gxx)) { throw "g++ not found: $Gxx" }
}

function Build-Test {
    Write-Step 'test: libjxl static-link smoke test'
    $src = Join-Path $SrcDir 'jxltest.c'
    $exe = Join-Path $OutDir 'jxltest.exe'
    $argList = $CommonFlags + @($src, '-o', $exe) + $JxlLibs + $StaticFlags
    if ((Invoke-Tool $Gxx $argList 'Link failed') -ne 0) { return 1 }
    Write-Ok "Built $exe"
    if (Test-Path $exe) {
        Write-Host '  --- Running ---'
        & $exe | Out-Host
        $runCode = $LASTEXITCODE
        Write-Host "  Exit code: $runCode"
        if ($runCode -ne 0) { return 1 }
    }
    return 0
}

function Build-Dll {
    Write-Step 'dll: artemis_jxl.dll'
    $out = Join-Path $OutDir 'artemis_jxl.dll'
    $sources = @('hook.cpp', 'jxl2png.cpp', 'av1_mft.cpp', 'thunk.S') |
        ForEach-Object { Join-Path $SrcDir $_ }
    foreach ($s in $sources) {
        if (-not (Test-Path $s)) { Write-Err "Missing source file $s"; return 1 }
    }

    $argList = $CommonFlags + @('-shared') + $sources +
               @('-o', $out) + $JxlLibs + $PngLibs + $Dav1dLibs +
               @('-lmfplat', '-lmfuuid', '-luuid', '-lole32', '-luser32') +
               $StaticFlags

    if ((Invoke-Tool $Gxx $argList 'DLL build failed') -ne 0) { return 1 }
    Write-Ok ("{0}  ({1:N0} bytes)" -f $out, (Get-Item $out).Length)
    return 0
}

function Build-Launcher {
    Write-Step 'launcher: launcher.exe (i686, GUI subsystem)'
    $out = Join-Path $OutDir 'launcher.exe'
    $src = Join-Path $SrcDir 'launcher.cpp'
    if (-not (Test-Path $src)) { Write-Err "Missing source file $src"; return 1 }

    # -municode for wWinMain, -mwindows so no console window pops up
    $argList = @('-O2', '-Wall', '-municode', '-mwindows') +
               @($src, '-o', $out) +
               @('-lshell32') + $StaticFlags

    if ((Invoke-Tool $Gxx $argList 'launcher build failed') -ne 0) { return 1 }
    Write-Ok ("{0}  ({1:N0} bytes)" -f $out, (Get-Item $out).Length)
    return 0
}

function Remove-Build {
    Write-Step 'clean'
    if (Test-Path $OutDir) {
        Get-ChildItem $OutDir -File | Remove-Item -Force
        Write-Ok "Cleared $OutDir"
    } else {
        Write-Warn2 'build directory does not exist, skipping'
    }
    return 0
}

New-Item -ItemType Directory -Force -Path $OutDir | Out-Null
Test-Prereq

$rc = 0
switch ($Target) {
    'test'     { $rc = Build-Test }
    'dll'      { $rc = Build-Dll }
    'launcher' { $rc = Build-Launcher }
    'clean'    { $rc = Remove-Build }
    'all'      { $rc = Build-Dll; if ($rc -eq 0) { $rc = Build-Launcher } }
}

Write-Host ''
if ($rc -eq 0) {
    Write-Host "Build complete (target=$Target)" -ForegroundColor Green
    Get-ChildItem $OutDir -File | Select-Object Name,
        @{n = 'Size'; e = { '{0:N0}' -f $_.Length } } | Format-Table -AutoSize
} else {
    Write-Host "Build failed (target=$Target, rc=$rc)" -ForegroundColor Red
}
exit $rc
