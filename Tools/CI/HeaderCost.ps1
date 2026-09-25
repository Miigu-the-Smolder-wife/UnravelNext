# Header coupling cost: which headers recompile how many translation units, and how many compile seconds that costs.
#   powershell -File Tools/CI/HeaderCost.ps1 [-BuildDir build/core] [-Top 30] [-Since "3 days ago"] [-External] [-Csv out.csv] [-Header <path>]
# Reads Ninja's dependency log (ninja -t deps: every object's headers as cl /showIncludes and the kernels' depfiles
# reported them, transitively) and .ninja_log (each output's last build duration). For each header: the objects and
# kernels that include it, the sum of their last compile times (what touching the header costs a build folder, as
# measured in that folder's last build, so under that build's parallelism), and the project headers that include it
# directly (where a split or forward declaration would cut the chain). "commits" counts the commits since -Since that
# changed the header, and "cost_s" = commits x compile_s is the compile time its edits cost one build folder over that
# period (the table is sorted by it: a header that recompiles everything but never changes costs nothing). -External lists system/SDK headers too (PCH
# candidates: those included by most objects). -Header <path> prints one header's dependents.
[CmdletBinding()]
param(
  [string]$BuildDir = "build/core",
  [int]$Top = 30,
  [string]$Since = "3 days ago",
  [switch]$External,
  [string]$Csv = "",
  [string]$Header = ""
)
$ErrorActionPreference = "Stop"
$root = (Split-Path -Parent (Split-Path -Parent (Split-Path -Parent $MyInvocation.MyCommand.Path))).ToLowerInvariant()
if (-not [IO.Path]::IsPathRooted($BuildDir)) { $BuildDir = Join-Path $root $BuildDir }
$BuildDir = (Resolve-Path $BuildDir).ProviderPath
$ninja = "C:\Program Files\Microsoft Visual Studio\18\Community\Common7\IDE\CommonExtensions\Microsoft\CMake\Ninja\ninja.exe"
if (-not (Test-Path (Join-Path $BuildDir ".ninja_deps"))) { throw "no .ninja_deps in $BuildDir (build it first)" }

function Norm([string]$p) {
  if (-not [IO.Path]::IsPathRooted($p)) { $p = Join-Path $BuildDir $p }
  try { $p = [IO.Path]::GetFullPath($p) } catch { }
  return $p.ToLowerInvariant()
}
function Rel([string]$p) { if ($p.StartsWith($root + "\")) { return $p.Substring($root.Length + 1).Replace('\', '/') } return $p }

# Last duration of each output (ms); .ninja_log keeps appending, so the last line of an output wins.
$seconds = @{}
foreach ($line in [IO.File]::ReadLines((Join-Path $BuildDir ".ninja_log"))) {
  if ($line.StartsWith("#")) { continue }
  $f = $line.Split("`t")
  if ($f.Count -lt 4) { continue }
  $seconds[(Norm $f[3])] = ([double]$f[1] - [double]$f[0]) / 1000.0
}

# Output -> headers (ninja -t deps), inverted to header -> outputs.
$dependents = @{}
$outputs = 0
$current = $null
foreach ($line in (& $ninja -C $BuildDir -t deps)) {
  if ($line -match '^(\S.*?): #deps (\d+),') {
    $current = Norm $Matches[1]
    if ($current -match '\.(obj|dxil)$') { $outputs++ } else { $current = $null }
    continue
  }
  if (-not $current) { continue }
  $h = $line.Trim()
  if (-not $h) { continue }
  $h = Norm $h
  $isProject = $h.StartsWith($root + "\") -and -not $h.StartsWith($root + "\external\") -and -not $h.StartsWith($BuildDir.ToLowerInvariant() + "\")
  if (-not $isProject -and -not $External) { continue }
  if (-not $dependents.ContainsKey($h)) { $dependents[$h] = New-Object System.Collections.Generic.List[string] }
  $dependents[$h].Add($current)
}

# Direct includers among project headers and sources (for where to cut).
function Get-DirectIncluders([string]$header) {
  $name = [IO.Path]::GetFileName($header)
  $hits = @()
  foreach ($f in $script:projectFiles) {
    if ($f.FullName.ToLowerInvariant() -eq $header) { continue }
    if (Select-String -Path $f.FullName -Pattern ('^\s*#\s*include\s*[<"]([^>"]*[/\\])?' + [regex]::Escape($name) + '[>"]') -Quiet) { $hits += (Rel $f.FullName.ToLowerInvariant()) }
  }
  return $hits
}
$projectFiles = @(Get-ChildItem -Path (Join-Path $root "Native"), (Join-Path $root "Tools"), (Join-Path $root "Reference"), (Join-Path $root "Tests") -Recurse -File -Include *.h, *.hpp, *.inl, *.hlsli -ErrorAction SilentlyContinue)

# Commits per file since -Since (paths relative to the repository, lower case).
$commits = @{}
foreach ($f in (& git -C $root log --since=$Since --name-only --format= 2>$null)) {
  if (-not $f) { continue }
  $k = $f.Trim().ToLowerInvariant()
  $commits[$k] = 1 + $(if ($commits.ContainsKey($k)) { $commits[$k] } else { 0 })
}

$rows = foreach ($kv in $dependents.GetEnumerator()) {
  $outs = @($kv.Value | Select-Object -Unique)
  $cpp = @($outs | Where-Object { $_.EndsWith(".obj") })
  $hlsl = @($outs | Where-Object { $_.EndsWith(".dxil") })
  $sec = 0.0
  foreach ($o in $outs) { if ($seconds.ContainsKey($o)) { $sec += $seconds[$o] } }
  $rel = Rel $kv.Key
  $n = if ($commits.ContainsKey($rel)) { $commits[$rel] } else { 0 }
  [pscustomobject]@{ header = $rel; objects = $cpp.Count; kernels = $hlsl.Count; compile_s = [math]::Round($sec, 1); commits = $n;
                     cost_s = [math]::Round($n * $sec, 0); full = $kv.Key }
}
$rows = @($rows | Sort-Object -Property @{ Expression = "cost_s"; Descending = $true }, @{ Expression = "compile_s"; Descending = $true })
$totalSec = 0.0
foreach ($kv in $seconds.GetEnumerator()) { if ($kv.Key -match '\.(obj|dxil)$') { $totalSec += $kv.Value } }

if ($Header) {
  $h = Norm (Join-Path $root $Header)
  if (-not $dependents.ContainsKey($h)) { throw "no output depends on $Header in $BuildDir" }
  "{0}: {1} outputs" -f (Rel $h), (@($dependents[$h] | Select-Object -Unique)).Count
  $dependents[$h] | Select-Object -Unique | ForEach-Object { "  {0,6:N1} s  {1}" -f $(if ($seconds.ContainsKey($_)) { $seconds[$_] } else { 0 }), (Rel $_) }
  "direct includers: " + ((Get-DirectIncluders $h) -join ", ")
  return
}

"{0}: {1} objects/kernels with dependency records, {2:N0} s of compile time in their last builds; commits since {3}" -f $BuildDir, $outputs, $totalSec, $Since
"{0,-60} {1,7} {2,7} {3,9} {4,7} {5,7} {6}" -f "header", "objects", "kernels", "compile_s", "commits", "cost_s", "direct includers (project)"
foreach ($r in ($rows | Select-Object -First $Top)) {
  $inc = if ($r.header.StartsWith("C:") -or $r.header.StartsWith("c:")) { "" } else { ((Get-DirectIncluders $r.full) | Select-Object -First 8) -join " " }
  "{0,-60} {1,7} {2,7} {3,9:N1} {4,7} {5,7} {6}" -f $r.header, $r.objects, $r.kernels, $r.compile_s, $r.commits, $r.cost_s, $inc
}
if ($Csv) { $rows | Select-Object header, objects, kernels, compile_s, commits, cost_s | Export-Csv -NoTypeInformation -Encoding UTF8 $Csv; "wrote $Csv" }
