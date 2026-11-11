$ErrorActionPreference = 'Stop'
$directory = Join-Path $PSScriptRoot 'build/dependencies'
New-Item -ItemType Directory -Path $directory -Force | Out-Null
$installer = Join-Path $directory 'PawnIO_setup.exe'
$hash = '1F519A22E47187F70A1379A48CA604981C4FCF694F4E65B734AAA74A9FBA3032'
if (!(Test-Path $installer) -or (Get-FileHash $installer).Hash -ne $hash) {
    curl.exe --fail --location --silent --show-error --max-time 60 --output $installer 'https://github.com/namazso/PawnIO.Setup/releases/download/2.2.0/PawnIO_setup.exe'
    if ($LASTEXITCODE -ne 0 -or (Get-FileHash $installer).Hash -ne $hash) { throw 'PawnIO installer verification failed.' }
}
if ((Get-AuthenticodeSignature $installer).Status -ne 'Valid') { throw 'PawnIO installer signature is invalid.' }
Get-Item $installer
