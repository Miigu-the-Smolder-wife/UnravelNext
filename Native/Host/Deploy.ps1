param(
  [string]$Build = "",                                      # default: build/I of this repository
  [string]$Bridge = "C:\Users\USER\Unravel\Assets\UnravelNextBridge"   # Unity bridge folder (old repository)
)
# Deploys UnravelNext.dll and the compiled kernels into the Unity bridge (I track):
#   <Bridge>/Plugins/x86_64/UnravelNext.dll (+ UnravelNext.dll.build.json: source commit, dirty flag, time)
#   <Bridge>/Native~/shaders/**.dxil          ('~' folder: Unity does not import it; Player builds copy it to
#                                              <Game>_Data/UnravelNext/shaders)
# Refuses while any process has the plugin loaded (a loaded DLL is never overwritten).
$ErrorActionPreference = "Stop"
$root = Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))
if (-not $Build) { $Build = Join-Path $root "build\I" }
$dll = Join-Path $Build "bin\UnravelNext.dll"
$shaders = Join-Path $Build "bin\shaders"
if (-not (Test-Path $dll)) { throw "missing $dll (Tools/CI/Build.ps1 -Track I -Target unx_host)" }
$target = Join-Path $Bridge "Plugins\x86_64\UnravelNext.dll"
$holders = @(Get-Process -ErrorAction SilentlyContinue | Where-Object {
  try { $_.Modules | Where-Object { $_.FileName -ieq $target } } catch { $null }
})
if ($holders.Count -gt 0) { throw "UnravelNext.dll is loaded by: $($holders.ProcessName -join ', ') (pid $($holders.Id -join ', ')); close it first" }
New-Item -ItemType Directory -Force (Split-Path $target) | Out-Null
Copy-Item $dll $target -Force
# Identity of the source the DLL was linked from (written next to it after a successful link, Native/Host/CMakeLists.txt).
$identityFile = "$dll.identity.h"
if (-not (Test-Path $identityFile) -or (Get-Item $identityFile).LastWriteTime -lt (Get-Item $dll).LastWriteTime) {
  throw "no build identity for $dll (rebuild unx_host: the identity is written after a successful link)"
}
$identity = Get-Content $identityFile -Raw
$commit = [regex]::Match($identity, 'UNX_BUILD_COMMIT "([0-9a-f]+)"').Groups[1].Value
$dirty = [regex]::Match($identity, 'UNX_BUILD_DIRTY (\d)').Groups[1].Value -eq "1"
$diff = [regex]::Match($identity, 'UNX_BUILD_DIFF_SHA256 "([0-9a-f]*)"').Groups[1].Value
@{ source = "UnravelNext"; commit = $commit; dirty = $dirty; diffSha256 = $diff; built = (Get-Item $dll).LastWriteTime.ToString("s"); deployed = (Get-Date).ToString("s") } |
  ConvertTo-Json | Set-Content -Encoding utf8 "$target.build.json"
$dest = Join-Path $Bridge "Native~\shaders"
if (Test-Path $dest) { Remove-Item -Recurse -Force $dest }
$count = 0
Get-ChildItem $shaders -Recurse -Filter *.dxil | ForEach-Object {
  $rel = $_.FullName.Substring($shaders.Length + 1)
  if ($rel -match '(^|\\)(Tests|Gates)\\') { return }   # test and gate kernels are not part of the runtime
  $out = Join-Path $dest $rel
  New-Item -ItemType Directory -Force (Split-Path $out) | Out-Null
  Copy-Item $_.FullName $out
  $count++
}
# Quality files (Config/quality): the renderer refuses to start without every key (no code defaults).
$qualityDest = Join-Path $Bridge "Native~\quality"
if (Test-Path $qualityDest) { Remove-Item -Recurse -Force $qualityDest }
New-Item -ItemType Directory -Force $qualityDest | Out-Null
Copy-Item (Join-Path $root "Config\quality\*.toml") $qualityDest
$qualityCount = (Get-ChildItem $qualityDest -Filter *.toml).Count
"deployed UnravelNext.dll ($commit$(if ($dirty) { ', dirty' })), $count kernels and $qualityCount quality files -> $Bridge"
