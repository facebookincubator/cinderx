param(
  [Parameter(Mandatory = $true)]
  [string]$RepoPath,

  [string]$UpstreamRemote = "origin",
  [string]$UpstreamBranch = "main",

  [string]$WorkBranch = "arm-jit",

  [switch]$NoRebase
)

Set-StrictMode -Version Latest
$ErrorActionPreference = "Stop"

function Exec {
  param([string]$Cmd)
  Write-Host ">> $Cmd"
  cmd.exe /c $Cmd
  if ($LASTEXITCODE -ne 0) { throw "Command failed ($LASTEXITCODE): $Cmd" }
}

if (-not (Test-Path $RepoPath)) {
  throw "RepoPath does not exist: $RepoPath"
}

Push-Location $RepoPath
try {
  Exec "git rev-parse --is-inside-work-tree"

  $status = (git status --porcelain)
  if ($status) {
    throw "Working tree is dirty in $RepoPath. Commit/stash first, then re-run."
  }

  Exec "git fetch $UpstreamRemote"

  # Update tracking branch to upstream (fast-forward only).
  Exec "git checkout $UpstreamBranch"
  Exec "git pull --ff-only $UpstreamRemote $UpstreamBranch"

  if ($NoRebase) {
    Write-Host "NoRebase set; fetched and fast-forwarded $UpstreamBranch only."
    exit 0
  }

  # Rebase work branch onto updated upstream branch.
  Exec "git checkout $WorkBranch"
  Exec "git rebase $UpstreamBranch"

  Write-Host "OK: $WorkBranch rebased onto $UpstreamBranch."
} finally {
  Pop-Location
}

