param(
    [Parameter(Mandatory)]
    [string] $LlvmMingw,
    [string] $OutputDirectory,
    [switch] $IncludePawnIOInstaller
)

$ErrorActionPreference = 'Stop'
$versionResource = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'version.rc') -Raw
if ($versionResource -notmatch '(?m)^#define COMPUTER_WATCH_VERSION "(V\d+\.\d+\.\d+)"') {
    throw 'Could not read the release version from version.rc.'
}
$version = $Matches[1]
$package = Join-Path $PSScriptRoot 'package'
$archiveRoot = if ($OutputDirectory) { $OutputDirectory } else { $package }
New-Item -ItemType Directory -Path $archiveRoot -Force | Out-Null
$machineIds = @{ x86 = 0x014c; x64 = 0x8664; arm64 = 0xaa64 }
$llvmRoot = (Resolve-Path -LiteralPath $LlvmMingw).Path
$gcc = Get-Item -LiteralPath (Get-Command g++.exe -CommandType Application | Select-Object -First 1).Source
$gccLicenses = Join-Path $gcc.Directory.Parent.FullName 'share/licenses'

$buildRoot = Join-Path $PSScriptRoot 'build'
if ($IncludePawnIOInstaller) {
    & (Join-Path $PSScriptRoot 'prepare-dependencies.ps1') | Out-Null
}
New-Item -ItemType Directory -Path $buildRoot -Force | Out-Null
$pawnSource = Join-Path $buildRoot 'PawnIO.Modules-0.2.11-source.tar.gz'
$pawnCommit = '52a7e536dff3e53c96917a28caac5e0fa6510696'
$pawnHash = 'C0D555C6C9C6F3803EDF28F5947AAEC82B0E240DFF94DD3313FA863D5A67D8AF'
if (!(Test-Path -LiteralPath $pawnSource) -or (Get-FileHash -LiteralPath $pawnSource).Hash -ne $pawnHash) {
    curl.exe --fail --location --silent --show-error --max-time 60 --output $pawnSource "https://codeload.github.com/namazso/PawnIO.Modules/tar.gz/$pawnCommit"
    if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $pawnSource).Hash -ne $pawnHash) {
        throw 'Could not verify the PawnIO.Modules source archive.'
    }
}
$pawnLicense = "PawnIO.Modules-$pawnCommit/COPYING"
tar.exe -xf $pawnSource -C $buildRoot $pawnLicense
if ($LASTEXITCODE -ne 0) { throw 'Could not extract the PawnIO.Modules license.' }

foreach ($architecture in 'x86', 'x64', 'arm64') {
    $exe = & (Join-Path $PSScriptRoot 'build.ps1') -Architecture $architecture -LlvmMingw $LlvmMingw
    $reader = [IO.BinaryReader]::new($exe.OpenRead())
    try {
        $reader.BaseStream.Position = 0x3c
        $reader.BaseStream.Position = $reader.ReadUInt32()
        if ($reader.ReadUInt32() -ne 0x4550 -or $reader.ReadUInt16() -ne $machineIds[$architecture]) {
            throw "Unexpected PE machine type for $architecture."
        }
    } finally {
        $reader.Dispose()
    }
    if ($exe.VersionInfo.ProductVersion -ne $version) {
        throw "Unexpected executable version for $architecture."
    }

    $name = "ComputerWatch-$version-windows-$architecture"
    if ($architecture -eq 'x64' -and $IncludePawnIOInstaller) {
        $name += '-pawnio-candidate'
    }
    $staging = Join-Path $PSScriptRoot "build/staging/$name"
    $payload = Join-Path $staging 'build'
    $licenses = Join-Path $staging 'licenses'
    New-Item -ItemType Directory -Path $payload, $licenses -Force | Out-Null
    Copy-Item -LiteralPath $exe.FullName, (Join-Path $exe.DirectoryName 'mcp_server.py') -Destination $payload -Force
    if ($architecture -eq 'x64' -and $IncludePawnIOInstaller) {
        $dependencies = Join-Path $payload 'dependencies'
        New-Item -ItemType Directory -Path $dependencies -Force | Out-Null
        Copy-Item -LiteralPath (Join-Path $buildRoot 'dependencies/PawnIO_setup.exe') -Destination $dependencies -Force
    }
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install.ps1'), (Join-Path $PSScriptRoot 'MCP.md') -Destination $staging -Force
    Copy-Item -LiteralPath (Join-Path $package 'README.md') -Destination $staging -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'LICENSE'), (Join-Path $PSScriptRoot 'THIRD-PARTY-NOTICES.md') -Destination $staging -Force
    Copy-Item -LiteralPath $pawnSource -Destination $licenses -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'MEMORY_TIMINGS.md') -Destination $staging -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'third_party/zenstates-core') -Destination $licenses -Recurse -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'assets/anonymous-pro/OFL.txt') -Destination (Join-Path $licenses 'Anonymous-Pro-OFL.txt') -Force
    Copy-Item -LiteralPath (Join-Path $buildRoot $pawnLicense) -Destination (Join-Path $licenses 'PawnIO.Modules-LGPL-2.1.txt') -Force
    if ($architecture -eq 'x64') {
        Copy-Item -LiteralPath (Join-Path $gccLicenses 'gcc-libs/COPYING.RUNTIME') -Destination (Join-Path $licenses 'GCC-Runtime-Exception.txt') -Force
        Copy-Item -LiteralPath (Join-Path $gccLicenses 'gcc-libs/COPYING3') -Destination (Join-Path $licenses 'GCC-GPL-3.0.txt') -Force
        Copy-Item -LiteralPath (Join-Path $gccLicenses 'crt/COPYING.MinGW-w64-runtime.txt') -Destination $licenses -Force
        Copy-Item -LiteralPath (Join-Path $gccLicenses 'libwinpthread/COPYING') -Destination (Join-Path $licenses 'winpthreads.txt') -Force
    } else {
        $target = @{ x86 = 'i686'; arm64 = 'aarch64' }[$architecture]
        $mingwLicenses = Join-Path $llvmRoot "$target-w64-mingw32/share/mingw32"
        Copy-Item -LiteralPath (Join-Path $llvmRoot 'LICENSE.TXT') -Destination (Join-Path $licenses 'LLVM.txt') -Force
        Copy-Item -LiteralPath (Join-Path $mingwLicenses 'COPYING.MinGW-w64-runtime.txt'), (Join-Path $mingwLicenses 'COPYING.winpthreads.txt') -Destination $licenses -Force
    }

    $archive = Join-Path $archiveRoot "$name.zip"
    Compress-Archive -LiteralPath $staging -DestinationPath $archive -Force
    Get-Item -LiteralPath $archive
}
