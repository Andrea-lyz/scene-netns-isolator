param(
    [string]$Out = "scene-netns-isolator-$((Get-Date).ToUniversalTime().AddHours(8).ToString('yyyyMMdd-HHmmss')).zip"
)

$ErrorActionPreference = "Stop"

$Root = Split-Path -Parent $PSScriptRoot
$Module = Join-Path $Root "module"
$Missing = @()

foreach ($Path in @(
    "bin\scene-netnsctl",
    "bin\su",
    "zygisk\arm64-v8a.so",
    "zygisk\armeabi-v7a.so"
)) {
    if (-not (Test-Path (Join-Path $Module $Path))) {
        $Missing += $Path
    }
}

if ($Missing.Count -gt 0) {
    Write-Error ("Missing built artifacts: " + ($Missing -join ", "))
    exit 1
}

$OutPath = Join-Path $Root $Out
if (Test-Path $OutPath) {
    Remove-Item -LiteralPath $OutPath
}

Add-Type -AssemblyName System.IO.Compression
Add-Type -AssemblyName System.IO.Compression.FileSystem

$Base = (Resolve-Path $Module).Path.TrimEnd('\', '/')
$Zip = [System.IO.Compression.ZipFile]::Open(
    $OutPath,
    [System.IO.Compression.ZipArchiveMode]::Create
)

try {
    Get-ChildItem -Path $Module -Recurse -File | ForEach-Object {
        $Relative = $_.FullName.Substring($Base.Length + 1).Replace('\', '/')
        [System.IO.Compression.ZipFileExtensions]::CreateEntryFromFile(
            $Zip,
            $_.FullName,
            $Relative,
            [System.IO.Compression.CompressionLevel]::Optimal
        ) | Out-Null
    }
} finally {
    $Zip.Dispose()
}

Write-Host "Wrote $OutPath"
