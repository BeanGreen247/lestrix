# Lestrix installer for Windows (PowerShell 5.1+).
#   .\install.ps1              install or update for the current user
#   .\install.ps1 -Uninstall   remove it
param([switch]$Uninstall)
$ErrorActionPreference = "Stop"

$Src     = Split-Path -Parent $MyInvocation.MyCommand.Path
$Data    = Join-Path $env:LOCALAPPDATA "Lestrix"
$Venv    = Join-Path $Data "venv"
$Menu    = Join-Path $env:APPDATA "Microsoft\Windows\Start Menu\Programs"
$Desktop = [Environment]::GetFolderPath("Desktop")

if ($Uninstall) {
    Remove-Item -Recurse -Force $Data -ErrorAction SilentlyContinue
    Remove-Item -Force (Join-Path $Menu "Lestrix.lnk"), (Join-Path $Desktop "Lestrix.lnk") -ErrorAction SilentlyContinue
    Write-Host "Lestrix removed. Saved connections were left alone."
    exit 0
}

# Python 3.10+ via the py launcher or python.exe
$py = $null
foreach ($cand in @("py -3", "python")) {
    try {
        $parts = $cand.Split(" ")
        & $parts[0] $parts[1..($parts.Length)] -c "import sys; sys.exit(sys.version_info < (3,10))" 2>$null
        if ($LASTEXITCODE -eq 0) { $py = $cand; break }
    } catch {}
}
if (-not $py) { throw "Python 3.10 or newer is required: winget install Python.Python.3.12" }

New-Item -ItemType Directory -Force $Data | Out-Null
$parts = $py.Split(" ")
& $parts[0] $parts[1..($parts.Length)] -m venv $Venv
& "$Venv\Scripts\python.exe" -m pip install --quiet --upgrade pip
& "$Venv\Scripts\python.exe" -m pip install --quiet --upgrade $Src
if ($LASTEXITCODE -ne 0) { throw "pip install failed" }

$assets = & "$Venv\Scripts\python.exe" -c "import lestrix, pathlib; print(pathlib.Path(lestrix.__file__).parent / 'assets')"
$shell = New-Object -ComObject WScript.Shell
foreach ($dir in @($Menu, $Desktop)) {
    $lnk = $shell.CreateShortcut((Join-Path $dir "Lestrix.lnk"))
    $lnk.TargetPath       = "$Venv\Scripts\pythonw.exe"   # pythonw: no console window
    $lnk.Arguments        = "-m lestrix"
    $lnk.WorkingDirectory = $env:USERPROFILE
    $lnk.IconLocation     = Join-Path $assets "lestrix.ico"
    $lnk.Description      = "SSH sessions, SFTP and local shells in one window"
    $lnk.Save()
}

if (-not (Get-Command ssh -ErrorAction SilentlyContinue)) {
    Write-Warning "OpenSSH client not found. Enable it: Settings > Apps > Optional features > OpenSSH Client."
}
Write-Host "Done. Start Lestrix from the Start menu."
Write-Host "Note: on Windows the SFTP browser needs a ControlMaster-capable ssh, so it is disabled there for now."
