param(
  # Git clone that tracks facebookincubator/cinderx and contains your ARM patch branch.
  [Parameter(Mandatory = $true)]
  [string]$RepoPath,

  [string]$UpstreamRemote = "origin",
  [string]$UpstreamBranch = "main",
  [string]$WorkBranch = "arm-jit",

  # ARM host to deploy+test on.
  [string]$ArmHost = "arm-host",
  [string]$User = "root",

  # Where the (non-git) working tree lives on ARM (rsync target).
  [string]$RemoteWorkDir = "/root/work/cinderx-main",
  [string]$RemoteIncomingDir = "/root/work/incoming",

  # Python used to build wheels on ARM.
  [string]$RemotePython = "/opt/python-3.14/bin/python3.14",

  # Driver venv used to run pyperformance on ARM.
  [string]$RemoteDriverVenv = "/root/venv-cinderx314",

  # pyperformance benchmark to run as a gate.
  [string]$Benchmark = "richards",

  # Auto-JIT threshold (compile after N calls).
  [int]$AutoJit = 50,

  # Keep builds stable on small ARM boxes.
  [int]$CmakeParallel = 1,

  [switch]$SkipPyperformance,
  [switch]$RecreatePyperfVenv
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Exec {
  param([string]$Cmd)
  Write-Host ">> $Cmd"
  cmd.exe /c $Cmd
  if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Cmd" }
}

function ExecPwsh {
  param([string]$Cmd)
  Write-Host ">> $Cmd"
  Invoke-Expression $Cmd
}

if (-not (Test-Path $RepoPath)) {
  throw "RepoPath does not exist: $RepoPath"
}

$timestamp = (Get-Date).ToString("yyyyMMdd_HHmmss")
$tmpDir = Join-Path $env:TEMP ("cinderx_arm_push_" + $timestamp)
New-Item -ItemType Directory -Force -Path $tmpDir | Out-Null
$tarPath = Join-Path $tmpDir ("cinderx-src_" + $timestamp + ".tar")

$sshOpts = "-o BatchMode=yes -o StrictHostKeyChecking=accept-new -o ConnectTimeout=10"

try {
  # 1) Sync/rebase (Windows only), then archive WorkBranch.
  $syncScript = Join-Path $PSScriptRoot "sync_upstream.ps1"
  if (-not (Test-Path $syncScript)) {
    throw "Missing script: $syncScript"
  }

  ExecPwsh ("& `"{0}`" -RepoPath `"{1}`" -UpstreamRemote `"{2}`" -UpstreamBranch `"{3}`" -WorkBranch `"{4}`"" -f `
      $syncScript, $RepoPath, $UpstreamRemote, $UpstreamBranch, $WorkBranch)

  Push-Location $RepoPath
  try {
    $workHead = (git rev-parse $WorkBranch).Trim()
    $upHead = (git rev-parse $UpstreamRemote/$UpstreamBranch).Trim()
    Write-Host ("WorkBranch {0} @ {1}" -f $WorkBranch, $workHead)
    Write-Host ("Upstream  {0}/{1} @ {2}" -f $UpstreamRemote, $UpstreamBranch, $upHead)

    Write-Host "Patch delta (stat):"
    git diff --stat "$UpstreamRemote/$UpstreamBranch...$WorkBranch" | ForEach-Object { Write-Host $_ }

    $changed = git diff --name-only "$UpstreamRemote/$UpstreamBranch...$WorkBranch"
    $jitChanged = $changed | Where-Object { $_ -match '^cinderx/Jit/' -or $_ -match '^cinderx/StaticPython/' }
    if ($jitChanged) {
      Write-Host "Changed JIT-related files:"
      $jitChanged | ForEach-Object { Write-Host ("  " + $_) }
    }

    # Only tracked files (no scratch/dist). Prefix makes extraction predictable.
    Exec ("git archive --format=tar --prefix=cinderx-src/ -o `"{0}`" {1}" -f $tarPath, $WorkBranch)
  } finally {
    Pop-Location
  }

  # 2) Copy archive to ARM.
  Exec ("ssh {0} {1}@{2} `"mkdir -p {3}`"" -f $sshOpts, $User, $ArmHost, $RemoteIncomingDir)
  Exec ("scp {0} `"{1}`" {2}@{3}:{4}/cinderx-update.tar" -f $sshOpts, $tarPath, $User, $ArmHost, $RemoteIncomingDir)

  # 3) Push the ARM helper script and execute it remotely (build + smoke + pyperformance gate).
  $remoteScript = Join-Path $PSScriptRoot "arm\\remote_update_build_test.sh"
  if (-not (Test-Path $remoteScript)) {
    throw "Missing remote helper script: $remoteScript"
  }

  Exec ("scp {0} `"{1}`" {2}@{3}:{4}/remote_update_build_test.sh" -f $sshOpts, $remoteScript, $User, $ArmHost, $RemoteIncomingDir)

  # Ensure LF line endings on the ARM host (Windows checkouts often use CRLF).
  Exec ("ssh {0} {1}@{2} `"tr -d '\\r' < {3}/remote_update_build_test.sh > {3}/remote_update_build_test.sh.lf && mv {3}/remote_update_build_test.sh.lf {3}/remote_update_build_test.sh`"" -f $sshOpts, $User, $ArmHost, $RemoteIncomingDir)

  $skip = $(if ($SkipPyperformance) { 1 } else { 0 })
  $recreate = $(if ($RecreatePyperfVenv) { 1 } else { 0 })

  $envPrefix = @(
    "INCOMING_DIR=$RemoteIncomingDir",
    "WORKDIR=$RemoteWorkDir",
    "PYTHON=$RemotePython",
    "DRIVER_VENV=$RemoteDriverVenv",
    "BENCH=$Benchmark",
    "AUTOJIT=$AutoJit",
    "PARALLEL=$CmakeParallel",
    "SKIP_PYPERF=$skip",
    "RECREATE_PYPERF_VENV=$recreate"
  ) -join " "

  $runCmd = @(
    "chmod +x $RemoteIncomingDir/remote_update_build_test.sh",
    "&&",
    "$envPrefix",
    "$RemoteIncomingDir/remote_update_build_test.sh"
  ) -join " "

  Exec ("ssh {0} {1}@{2} `"{3}`"" -f $sshOpts, $User, $ArmHost, $runCmd)
} finally {
  if (Test-Path $tmpDir) {
    Remove-Item -Recurse -Force $tmpDir | Out-Null
  }
}
