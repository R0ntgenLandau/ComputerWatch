$ErrorActionPreference = 'Stop'
$compiler = (Get-Command g++.exe -CommandType Application | Select-Object -First 1).Source
$resourceCompiler = (Get-Command windres.exe -CommandType Application | Select-Object -First 1).Source
$build = Join-Path $PSScriptRoot 'build'
New-Item -ItemType Directory -Path $build -Force | Out-Null

$module = Join-Path $build 'IntelMSR.bin'
$moduleHash = 'D6ED85D65AB17A22F813EF98207D6D537155EE2DED5976A21CB48413C9B92E5F'
if (!(Test-Path -LiteralPath $module) -or (Get-FileHash -LiteralPath $module).Hash -ne $moduleHash) {
    $archive = Join-Path $build 'PawnIO-modules-0.2.11.zip'
    curl.exe --fail --location --silent --show-error --max-time 60 --output $archive 'https://github.com/namazso/PawnIO.Modules/releases/download/0.2.11/release_0_2_11.zip'
    $archiveHash = '43608CB89BC84247FEF1368A139013F7D043E17DB6D6C8DFC9B46BF0905A81F4'
    if ($LASTEXITCODE -ne 0 -or (Get-FileHash -LiteralPath $archive).Hash -ne $archiveHash) {
        throw 'The official PawnIO module archive could not be verified.'
    }
    Add-Type -AssemblyName System.IO.Compression.FileSystem
    $zip = [IO.Compression.ZipFile]::OpenRead($archive)
    try {
        $entry = $zip.GetEntry('IntelMSR.bin')
        if ($null -eq $entry) { throw 'IntelMSR.bin is missing from the module archive.' }
        [IO.Compression.ZipFileExtensions]::ExtractToFile($entry, $module, $true)
    } finally {
        $zip.Dispose()
    }
    if ((Get-FileHash -LiteralPath $module).Hash -ne $moduleHash) { throw 'IntelMSR.bin hash mismatch.' }
}

Push-Location -LiteralPath $build
try {
    @(
        '1 24 "../ComputerWatch.manifest"'
        '1 ICON "../ComputerWatch.ico"'
        '101 RCDATA "IntelMSR.bin"'
    ) | Set-Content -LiteralPath 'resources.rc' -Encoding Ascii
    & $resourceCompiler -i resources.rc -o resources.o -O coff
    if ($LASTEXITCODE -ne 0) { throw 'Resource compilation failed.' }

    $sources = @('main.cpp', 'telemetry.cpp', 'disk_smart.cpp', 'history.cpp', 'ui.cpp') |
        ForEach-Object { Join-Path $PSScriptRoot $_ }
    & $compiler -std=c++20 -O2 -Wall -Wextra -Wpedantic -finput-charset=UTF-8 -DUNICODE -D_UNICODE -DNOMINMAX -D_WIN32_WINNT=0x0A00 -static -municode -mwindows @sources resources.o -o ComputerWatch.exe -lcomctl32 -lgdi32 -lshell32 -ladvapi32 -lpdh -liphlpapi -lpsapi -lole32 -ldwmapi -luxtheme -lsetupapi
    if ($LASTEXITCODE -ne 0) { throw 'ComputerWatch compilation failed.' }
} finally {
    Pop-Location
}
Get-Item -LiteralPath (Join-Path $build 'ComputerWatch.exe')
