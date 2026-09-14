$ErrorActionPreference = 'Stop'
$root = Split-Path $PSScriptRoot -Parent
$version = (Get-Content (Join-Path $root 'VERSION') -Raw).Trim()
if ($version -notmatch '^\d+\.\d+\.\d+$') { throw 'VERSION format' }
$files = @('CCCaster_B.exe', 'CCCaster_B_GUI.exe', 'libcccaster_hook.dll')
$stage = Join-Path $PSScriptRoot ('staging/' + [guid]::NewGuid().ToString() + '/cccaster_B')
New-Item -ItemType Directory -Path $stage -Force | Out-Null
foreach ($name in $files) {
    $source = Join-Path $root "build/bin/$name"
    $bytes = [IO.File]::ReadAllBytes($source)
    $pe = [BitConverter]::ToInt32($bytes, 60)
    if ([BitConverter]::ToUInt16($bytes, $pe + 4) -ne 0x14c) { throw "Not 32bit: $name" }
    Copy-Item -LiteralPath $source -Destination $stage
}
$hashes = $files | ForEach-Object { (Get-FileHash (Join-Path $stage $_)).Hash + '  ' + $_ }
$hashes | Set-Content (Join-Path $stage 'SHA256SUMS.txt') -Encoding ascii
$zip = Join-Path $PSScriptRoot "packages/CCCaster_verB-$version.zip"
if (Test-Path -LiteralPath $zip) { throw "Package already exists: $zip" }
Compress-Archive -Path $stage -DestinationPath $zip
Write-Output $zip
