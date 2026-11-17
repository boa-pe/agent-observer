[CmdletBinding()]
param(
    [string]$OutputDirectory = 'build\web-flasher'
)

$ErrorActionPreference = 'Stop'
$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot '..')).Path
$BuildDirectory = Join-Path $ProjectRoot 'build'
$ExportScript = $null

if ($env:IDF_PATH) {
    $ConfiguredExportScript = Join-Path $env:IDF_PATH 'export.ps1'
    if (Test-Path $ConfiguredExportScript) {
        $ExportScript = $ConfiguredExportScript
    }
}

if (-not $ExportScript) {
    $SettingsPath = Join-Path $ProjectRoot '.vscode\settings.json'
    if (Test-Path $SettingsPath) {
        $Settings = Get-Content $SettingsPath -Raw | ConvertFrom-Json
        $ConfiguredIdfPath = $Settings.'idf.currentSetup'
        if ($ConfiguredIdfPath) {
            $ConfiguredExportScript = Join-Path $ConfiguredIdfPath 'export.ps1'
            if (Test-Path $ConfiguredExportScript) {
                $ExportScript = $ConfiguredExportScript
            }
        }
    }
}

if (-not $ExportScript) {
    throw 'ESP-IDF was not found. Set IDF_PATH or configure idf.currentSetup in .vscode/settings.json.'
}

$IdfPath = Split-Path $ExportScript -Parent
if (-not $env:IDF_TOOLS_PATH) {
    $EspressifPath = Split-Path (Split-Path $IdfPath -Parent) -Parent
    if (Test-Path (Join-Path $EspressifPath 'python_env')) {
        $env:IDF_TOOLS_PATH = $EspressifPath
    }
}

$Python = Get-Command python -ErrorAction SilentlyContinue
if (-not $Python) {
    throw 'Python was not found. Install ESP-IDF tools before preparing web flasher files.'
}

Write-Host "Activating ESP-IDF from: $IdfPath"
$ActivationCommands = & $Python.Source (Join-Path $IdfPath 'tools\activate.py') --export
if ($LASTEXITCODE -ne 0) {
    throw "ESP-IDF activation failed. Run '& `"$IdfPath\install.ps1`" esp32' once to install the required tools, then retry."
}

. ([System.Management.Automation.ScriptBlock]::Create(($ActivationCommands -join [Environment]::NewLine)))

if (-not (Get-Command idf.py -ErrorAction SilentlyContinue)) {
    throw 'ESP-IDF activation finished, but idf.py is not available in this PowerShell session.'
}

if ([System.IO.Path]::IsPathRooted($OutputDirectory)) {
    $OutputPath = $OutputDirectory
} else {
    $OutputPath = Join-Path $ProjectRoot $OutputDirectory
}

Push-Location $ProjectRoot
try {
    & idf.py build
    if ($LASTEXITCODE -ne 0) {
        throw "idf.py build failed with exit code $LASTEXITCODE"
    }
} finally {
    Pop-Location
}

$FlasherArgsPath = Join-Path $BuildDirectory 'flasher_args.json'
if (-not (Test-Path $FlasherArgsPath)) {
    throw "ESP-IDF did not generate $FlasherArgsPath"
}

$FlasherArgs = Get-Content $FlasherArgsPath -Raw | ConvertFrom-Json
$FlashFiles = @(
    $FlasherArgs.flash_files.PSObject.Properties | ForEach-Object {
        [PSCustomObject]@{
            Offset = $_.Name
            Source = Join-Path $BuildDirectory ($_.Value -replace '/', '\')
            Name = [System.IO.Path]::GetFileName($_.Value)
        }
    } | Sort-Object { [Convert]::ToUInt32($_.Offset.Substring(2), 16) }
)

if ($FlashFiles.Count -eq 0) {
    throw 'ESP-IDF did not list any flash files in flasher_args.json'
}

$DuplicateNames = $FlashFiles | Group-Object Name | Where-Object Count -gt 1
if ($DuplicateNames) {
    throw "Flash output contains duplicate file names: $($DuplicateNames.Name -join ', ')"
}

New-Item -ItemType Directory -Force -Path $OutputPath | Out-Null
foreach ($File in $FlashFiles) {
    if (-not (Test-Path $File.Source)) {
        throw "Required flash image was not generated: $($File.Source)"
    }
    Copy-Item -LiteralPath $File.Source -Destination (Join-Path $OutputPath $File.Name) -Force
}

$Instructions = @(
    "Target: $($FlasherArgs.extra_esptool_args.chip)"
    "Flash mode: $($FlasherArgs.flash_settings.flash_mode)"
    "Flash size: $($FlasherArgs.flash_settings.flash_size)"
    "Flash frequency: $($FlasherArgs.flash_settings.flash_freq)"
    ''
    'In ESP Web Tool, add each .bin file below and set its address:'
)
$Instructions += $FlashFiles | ForEach-Object { "$($_.Offset)  $($_.Name)" }
$Instructions += @(
    ''
    'Connect the board, select the serial port, add the files with these addresses,'
    'then click Program. Use a desktop Chromium-based browser with Web Serial support.'
)
$Instructions | Set-Content -LiteralPath (Join-Path $OutputPath 'flash-addresses.txt') -Encoding UTF8

Write-Host "ESP Web Tool files are ready in: $OutputPath"
