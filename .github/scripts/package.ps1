param([string]$SmokeExecutable)

$ErrorActionPreference = 'Stop'
$PSNativeCommandUseErrorActionPreference = $true

function Set-CleanRuntime {
  Get-ChildItem Env: | Where-Object { $_.Name -match '^(QT|QML|LD_)' } |
    ForEach-Object { Remove-Item -LiteralPath "Env:$($_.Name)" }
  if ($IsWindows) {
    $env:PATH = "$env:SystemRoot\System32;$env:SystemRoot"
  } else {
    $env:PATH = '/usr/bin:/bin'
  }
}

function Test-LinuxDependencies {
  param([string]$Executable, [string]$Extracted, [bool]$Gui)

  $output = (& ldd $Executable | Out-String)
  if ($output -match '=>\s+not found') { throw $output }
  $qt = [regex]::Matches($output, '(?m)^\s*libQt6\S*\s+=>\s+(\S+)')
  if (-not $Gui -and $qt.Count -ne 0) { throw "CLI unexpectedly links to Qt:`n$output" }
  if ($Gui) {
    $prefix = "$((Resolve-Path -LiteralPath $Extracted).Path)/"
    if ($qt.Count -eq 0) { throw "GUI Qt libraries not found:`n$output" }
    foreach ($match in $qt) {
      if (-not $match.Groups[1].Value.StartsWith($prefix, [StringComparison]::Ordinal)) {
        throw "GUI loaded Qt from outside its archive:`n$output"
      }
    }
  }
}

function Test-WindowVisible {
  param($Process)

  if ($IsWindows) {
    $Process.Refresh()
    return $Process.MainWindowTitle -ceq 'PackStacker'
  }
  $PSNativeCommandUseErrorActionPreference = $false
  xdotool search --all --onlyvisible --pid $Process.Id --name '^PackStacker$' *> $null
  return $LASTEXITCODE -eq 0
}

function Invoke-GuiSmoke {
  param([string]$Executable, [string]$LogDirectory)

  $env:QT_QUICK_BACKEND = 'software'
  $env:QT_QPA_PLATFORM = if ($IsWindows) { 'windows' } else { 'xcb' }
  $stdout = Join-Path $LogDirectory gui.stdout
  $stderr = Join-Path $LogDirectory gui.stderr
  $process = Start-Process -FilePath $Executable -WorkingDirectory (Split-Path -Parent $Executable) `
    -RedirectStandardOutput $stdout -RedirectStandardError $stderr -PassThru
  try {
    $timer = [Diagnostics.Stopwatch]::StartNew()
    while (-not $process.HasExited -and $timer.Elapsed.TotalSeconds -lt 25) {
      if (Test-WindowVisible $process) { return }
      Start-Sleep -Milliseconds 200
    }
    Get-Content -LiteralPath $stdout, $stderr
    throw 'PackStacker window did not appear'
  } finally {
    if (-not $process.HasExited) { $process.Kill() }
    if (-not $process.WaitForExit(10000)) { throw 'PackStacker did not stop' }
    $process.Dispose()
  }
}

function Invoke-CliSmoke {
  param([string]$Executable, [string]$Directory)

  $inputPack = Join-Path $Directory 'Входной пак'
  $outputPack = Join-Path $Directory 'Готовый пак.zip'
  New-Item -ItemType Directory -Path $inputPack | Out-Null
  Set-Content -LiteralPath "$inputPack/pack.mcmeta" -Encoding ascii `
    -Value '{"pack":{"pack_format":34,"description":"CI smoke"}}'
  $payload = [byte[]](0, 10, 13, 26, 255)
  [IO.File]::WriteAllBytes("$inputPack/payload.bin", $payload)
  $start = [Diagnostics.ProcessStartInfo]::new($Executable)
  $start.WorkingDirectory = Split-Path -Parent $Executable
  foreach ($argument in @(
    '--type', 'resource', '--target', '34', '--output-format', 'zip', '--output', $outputPack, $inputPack
  )) {
    $start.ArgumentList.Add($argument)
  }
  $process = [Diagnostics.Process]::Start($start)
  try {
    if (-not $process.WaitForExit(15000)) { throw 'PackStacker CLI timed out' }
    if ($process.ExitCode -ne 0) { throw "PackStacker CLI failed: $($process.ExitCode)" }
  } finally {
    if (-not $process.HasExited) {
      $process.Kill()
      if (-not $process.WaitForExit(5000)) { throw 'PackStacker CLI did not stop' }
    }
    $process.Dispose()
  }
  $roundTrip = Join-Path $Directory roundtrip
  Expand-Archive -LiteralPath $outputPack -DestinationPath $roundTrip
  if ([Convert]::ToHexString([IO.File]::ReadAllBytes("$roundTrip/payload.bin")) -cne
      [Convert]::ToHexString($payload)) {
    throw 'PackStacker CLI did not preserve the binary payload'
  }
}

if ($SmokeExecutable) {
  Set-CleanRuntime
  Invoke-GuiSmoke $SmokeExecutable (Split-Path -Parent $SmokeExecutable)
  exit 0
}

$pwsh = (Get-Process -Id $PID).Path
$gui = $env:CI_PRESET -eq 'release'
$suffix = if ($IsWindows) { '.exe' } else { '' }
$extension = if ($IsWindows) { '.zip' } else { '.tar.gz' }
$tar = if ($IsWindows) { Join-Path $env:SystemRoot System32/tar.exe } else { 'tar' }
$archive = Join-Path $env:RUNNER_TEMP "$env:ARTIFACT_NAME$extension"
$work = Join-Path $env:RUNNER_TEMP ([guid]::NewGuid().ToString())
$install = Join-Path $work install
$extracted = Join-Path $work extracted
New-Item -ItemType Directory -Path $extracted | Out-Null
try {
  cmake --install "build/$env:CI_PRESET" --prefix $install
  if ($IsWindows) {
    & $tar -acf $archive -C $install .
  } else {
    & $tar -czf $archive -C $install .
  }
  & $tar -xf $archive -C $extracted
  $cli = Join-Path $extracted "bin/packstacker-cli$suffix"
  $graphical = Join-Path $extracted "bin/packstacker-gui$suffix"
  if ((Test-Path -LiteralPath $graphical -PathType Leaf) -ne $gui) {
    throw 'Wrong GUI executable presence for package profile'
  }
  Set-CleanRuntime
  if (-not $IsWindows) {
    Test-LinuxDependencies $cli $extracted $false
    if ($gui) { Test-LinuxDependencies $graphical $extracted $true }
  }
  $version = @(& $cli --version)
  if ($version.Count -ne 1 -or $version[0] -cne "PackStacker $env:PACKSTACKER_VERSION") {
    throw "Unexpected packaged CLI version: $version"
  }
  Invoke-CliSmoke $cli $work
  if ($gui -and $env:GITHUB_REF_TYPE -eq 'tag') {
    if ($IsWindows) {
      Invoke-GuiSmoke $graphical $work
    } else {
      timeout 45s xvfb-run -a $pwsh -NoProfile -File $PSCommandPath -SmokeExecutable $graphical
    }
  }
  "PACKSTACKER_ARCHIVE=$archive" >> $env:GITHUB_ENV
  Write-Host "Verified archive: $archive"
} finally {
  Remove-Item -LiteralPath $work -Recurse -Force
}
