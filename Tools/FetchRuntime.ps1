<#
Downloads the pinned portable Python (python-build-standalone) and uv into ThirdParty/, verifying SHA-256.
Release archives already include these; run this after cloning the repo.

  powershell -ExecutionPolicy Bypass -File Tools/FetchRuntime.ps1                 # Win64 + Linux
  powershell -ExecutionPolicy Bypass -File Tools/FetchRuntime.ps1 -Platforms Win64
#>
param(
    [string[]]$Platforms = @('Win64', 'Linux'),
    [switch]$Force
)
$ErrorActionPreference = 'Stop'
$ProgressPreference = 'SilentlyContinue'

$PluginRoot = Split-Path -Parent $PSScriptRoot
$ThirdParty = Join-Path $PluginRoot 'ThirdParty'
$Downloads = Join-Path $ThirdParty '.downloads'

$PbsTag = '20261009'
$PythonVersion = '3.12.15'
$UvVersion = '0.13.0'
$Pbs = "https://github.com/astral-sh/python-build-standalone/releases/download/$PbsTag"
$Uv = "https://github.com/astral-sh/uv/releases/download/$UvVersion"

$Artifacts = @{
    Win64 = @{
        Python = @{ File = "cpython-$PythonVersion+$PbsTag-x86_64-pc-windows-msvc-install_only_stripped.tar.gz"; Sha256 = '11ed9f8a6ac64fb1b5a55e40e305a6744a3f77a70863c8f8197a6e4349abc64a'; Check = 'python.exe' }
        Uv     = @{ File = 'uv-x86_64-pc-windows-msvc.zip'; Sha256 = '088962f9e7b7bd9ea740c04c650b2a21c8928c345bd99ac24350dc924dba656c'; Check = 'uv.exe'; Strip = 0 }
    }
    Linux = @{
        Python = @{ File = "cpython-$PythonVersion+$PbsTag-x86_64-unknown-linux-gnu-install_only_stripped.tar.gz"; Sha256 = 'ffa8f85f1b56e88b687f08d743d817015524b86a9365fd921243132b80f1a36d'; Check = 'bin/python3.12' }
        Uv     = @{ File = 'uv-x86_64-unknown-linux-gnu.tar.gz'; Sha256 = '1468ebd5a5541121837c5a2817b9972ba6090fa6caa3d142620850a47fb75154'; Check = 'uv' }
    }
}

function Get-Verified($Url, $File, $Sha256) {
    New-Item -ItemType Directory -Force $Downloads | Out-Null
    $Path = Join-Path $Downloads $File
    if (-not (Test-Path $Path) -or (Get-FileHash $Path -Algorithm SHA256).Hash -ne $Sha256.ToUpper()) {
        Write-Host "  downloading $File"
        Invoke-WebRequest -Uri ($Url + '/' + [uri]::EscapeDataString($File)) -OutFile $Path -UseBasicParsing
    }
    $Actual = (Get-FileHash $Path -Algorithm SHA256).Hash
    if ($Actual -ne $Sha256.ToUpper()) { throw "SHA-256 mismatch for ${File}: got $Actual" }
    return $Path
}

function Expand-Into($Archive, $Dest, $Check, $Strip = 1) {
    if (Test-Path $Dest) { Remove-Item -Recurse -Force $Dest }
    New-Item -ItemType Directory -Force $Dest | Out-Null
    # Windows' own bsdtar (not a GNU tar from Git/MSYS on PATH, which reads "C:" as a remote host). It
    # can't create the Linux archives' symlinks (python3 -> python3.12, libpython*.so); the real files
    # extract fine and the runtime launches them directly, so those errors are expected and ignored.
    $Tar = Join-Path $env:SystemRoot 'System32\tar.exe'
    $Previous = $ErrorActionPreference
    $ErrorActionPreference = 'Continue'
    & $Tar -xf $Archive -C $Dest "--strip-components=$Strip" 2>&1 | Out-Null
    $ErrorActionPreference = $Previous
    if (-not (Test-Path (Join-Path $Dest $Check))) { throw "extracting $Archive did not produce $Check" }
}

foreach ($Platform in $Platforms) {
    $A = $Artifacts[$Platform]
    if (-not $A) { throw "unknown platform $Platform (Win64, Linux)" }
    $PyDir = Join-Path $ThirdParty "python/$Platform"
    $UvDir = Join-Path $ThirdParty "uv/$Platform"
    $Stamp = Join-Path $PyDir '.runtime-version'
    $Want = "python $PythonVersion ($PbsTag), uv $UvVersion"
    if (-not $Force -and (Test-Path $Stamp) -and (Get-Content $Stamp -Raw).Trim() -eq $Want -and (Test-Path (Join-Path $UvDir $A.Uv.Check))) {
        Write-Host "$Platform up to date: $Want"
        continue
    }
    Write-Host "$Platform"
    Expand-Into (Get-Verified $Pbs $A.Python.File $A.Python.Sha256) $PyDir $A.Python.Check
    Expand-Into (Get-Verified $Uv $A.Uv.File $A.Uv.Sha256) $UvDir $A.Uv.Check $(if ($null -ne $A.Uv.Strip) { $A.Uv.Strip } else { 1 })
    Set-Content -Path $Stamp -Value $Want -NoNewline
    Write-Host "  -> $PyDir, $UvDir"
}
