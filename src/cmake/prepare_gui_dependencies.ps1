param([Parameter(Mandatory=$true)][string]$BuildDirectory)
$ErrorActionPreference = 'Stop'
[Net.ServicePointManager]::SecurityProtocol = [Net.SecurityProtocolType]::Tls12
$taskBuild = [IO.Path]::GetFullPath($BuildDirectory)
$taskDeps = Join-Path $taskBuild 'gui-deps'
$taskManifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'gui-dependencies.json') -Raw | ConvertFrom-Json
New-Item -ItemType Directory -Path $taskDeps -Force | Out-Null
foreach ($entry in $taskManifest.files) {
    $path = Join-Path $taskDeps $entry.name
    if (!(Test-Path -LiteralPath $path)) {
        Write-Host "GUI dependency: $($entry.name)"
        $download = "$path.download"
        Invoke-WebRequest -UseBasicParsing -Uri $entry.url -OutFile $download
        if ((Get-FileHash -LiteralPath $download -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Download hash mismatch: $($entry.name)" }
        Move-Item -LiteralPath $download -Destination $path -Force
    }
    if ((Get-FileHash -LiteralPath $path -Algorithm SHA256).Hash -ne $entry.sha256) { throw "Dependency hash mismatch: $path" }
}
# 描画エンジンはソースから静的リンクする。外部のブラウザーは配布しない。
foreach ($entry in @(@{ archive='rmlui-6.3.zip'; directory=('RmlUi-' + $taskManifest.rmluiCommit) }, @{ archive='freetype-2.13.3.zip'; directory='freetype-VER-2-13-3' })) {
    if (!(Test-Path -LiteralPath (Join-Path $taskDeps ($entry.directory + '/CMakeLists.txt')))) {
        Expand-Archive -LiteralPath (Join-Path $taskDeps $entry.archive) -DestinationPath $taskDeps -Force
    }
}
Write-Host "Pinned GUI sources ready: RmlUi $($taskManifest.rmluiVersion), FreeType $($taskManifest.freetypeVersion)"
$taskOutput = Join-Path $taskBuild 'bin/gui-licenses'
New-Item -ItemType Directory -Path $taskOutput -Force | Out-Null
Copy-Item -LiteralPath (Join-Path $taskDeps ('RmlUi-' + $taskManifest.rmluiCommit + '/LICENSE.txt')) -Destination (Join-Path $taskOutput 'RmlUi-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $taskDeps ('RmlUi-' + $taskManifest.rmluiCommit + '/Include/RmlUi/Core/Containers/LICENSE.txt')) -Destination (Join-Path $taskOutput 'RmlUi-Containers-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $taskDeps ('RmlUi-' + $taskManifest.rmluiCommit + '/Backends/RmlUi_DirectX/LICENSE.txt')) -Destination (Join-Path $taskOutput 'RmlUi-DirectX-LICENSE.txt') -Force
Copy-Item -LiteralPath (Join-Path $taskDeps 'freetype-VER-2-13-3/docs/FTL.TXT') -Destination (Join-Path $taskOutput 'FreeType-FTL.txt') -Force
Copy-Item -LiteralPath (Join-Path $taskDeps 'OFL.txt') -Destination (Join-Path $taskOutput 'NotoSansJP-OFL.txt') -Force
Copy-Item -LiteralPath (Join-Path $PSScriptRoot 'gui-dependencies.json') -Destination $taskOutput -Force
$taskFiles = @(Get-ChildItem -LiteralPath $taskOutput -File | Where-Object { $_.Name -ne 'files.sha256.json' } | ForEach-Object {
    @{ path = $_.Name; sha256 = (Get-FileHash -LiteralPath $_.FullName -Algorithm SHA256).Hash.ToLowerInvariant() }
})
@{ version = ('RmlUi-' + $taskManifest.rmluiVersion); files = $taskFiles } | ConvertTo-Json -Depth 4 | Set-Content -LiteralPath (Join-Path $taskOutput 'files.sha256.json') -Encoding UTF8
