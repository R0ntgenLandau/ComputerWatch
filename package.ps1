param(
    [Parameter(Mandatory)]
    [string] $LlvmMingw
)

$ErrorActionPreference = 'Stop'
$versionResource = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'version.rc') -Raw
if ($versionResource -notmatch '(?m)^#define COMPUTER_WATCH_VERSION "(V\d+\.\d+\.\d+)"') {
    throw 'Could not read the release version from version.rc.'
}
$version = $Matches[1]
$package = Join-Path $PSScriptRoot 'package'
$machineIds = @{ x86 = 0x014c; x64 = 0x8664; arm64 = 0xaa64 }

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
    $staging = Join-Path $PSScriptRoot "build/staging/$name"
    $payload = Join-Path $staging 'build'
    New-Item -ItemType Directory -Path $payload -Force | Out-Null
    Copy-Item -LiteralPath $exe.FullName, (Join-Path $exe.DirectoryName 'mcp_server.py') -Destination $payload -Force
    Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'install.ps1'), (Join-Path $PSScriptRoot 'MCP.md') -Destination $staging -Force
    Copy-Item -LiteralPath (Join-Path $package 'README.md') -Destination $staging -Force

    $archive = Join-Path $package "$name.zip"
    Compress-Archive -LiteralPath $staging -DestinationPath $archive -Force
    Get-Item -LiteralPath $archive
}
