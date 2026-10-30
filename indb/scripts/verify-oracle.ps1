#  Copyright (c) 2026 The in-database time-series clustering authors.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
#
# Stage 4 of docs/verification-plan.md: compare the C++ kernel against the
# original Java implementation that ships with IoTDB.
#
# K-Shape and Medoid-Shape start from a random assignment, so two correct
# implementations do not produce identical numbers; they produce the same
# distribution of optima. The test therefore measures how much the reference
# itself varies between runs and requires the C++ result to fall inside that
# spread, and to be no less stable than the reference.
#
#   powershell -ExecutionPolicy Bypass -File indb/scripts/verify-oracle.ps1
#
param(
  [string]$RocksDbCli = 'E:\build\indb\Release\indb_cli.exe',
  [string]$JavaHome = 'E:\java',
  [string]$UdfJar = 'E:\实验\work\environment\iotdb-0.13.3-clean\ext\udf\library-udf-0.14.0-SNAPSHOT-jar-with-dependencies.jar',
  [string]$WorkDir = 'E:\tmp\indb-oracle',
  [int]$L = 64,
  [int]$K = 3,
  [int]$Edk = 5,
  [int]$Copies = 50,
  [int]$JavaRuns = 5,
  [int]$CppSeeds = 3,
  [double]$Tolerance = 1e-6
)

$ErrorActionPreference = 'Continue'
$script:Passed = 0
$script:Failed = 0
$script:Failures = @()

function Check {
  param([string]$Name, [bool]$Condition, [string]$Detail = '')
  if ($Condition) {
    $script:Passed++
    Write-Host ("  PASS  {0}" -f $Name)
  } else {
    $script:Failed++
    $script:Failures += $Name
    Write-Host ("  FAIL  {0} {1}" -f $Name, $Detail)
  }
}

function Read-CenterFile {
  param([string]$Path)
  $lines = @(Get-Content $Path)
  if ($lines.Count -lt 1) { return @() }
  $header = @(($lines[0] -split '\s+') | Where-Object { $_ -ne '' })
  if ($header.Count -lt 2) { return @() }
  $rows = [int]$header[0]
  $cols = [int]$header[1]
  $values = @(($lines | Select-Object -Skip 1) -join ' ' -split '\s+' |
              Where-Object { $_ -ne '' } | ForEach-Object { [double]$_ })
  if ($values.Count -ne $rows * $cols) { return @() }
  return $values
}

# Parameter names stay distinct from the loop variables: in PowerShell $a and
# $A are the same variable, which silently corrupts the array being indexed.
function Row-Distance {
  param([double[]]$RowA, [double[]]$RowB, [int]$OffsetA, [int]$OffsetB, [int]$Length)
  $same = 0.0
  $flipped = 0.0
  for ($i = 0; $i -lt $Length; $i++) {
    $va = $RowA[$OffsetA + $i]
    $vb = $RowB[$OffsetB + $i]
    $d1 = [Math]::Abs($va - $vb)
    $d2 = [Math]::Abs($va + $vb)
    if ($d1 -gt $same) { $same = $d1 }
    if ($d2 -gt $flipped) { $flipped = $d2 }
  }
  return [Math]::Min($same, $flipped)
}

# Greedy nearest matching between two k x l centre matrices. Cluster order and
# eigenvector sign are arbitrary, so only the matched distance is meaningful.
function Compare-Centers {
  param([double[]]$Left, [double[]]$Right, [int]$Count, [int]$Length)
  $used = New-Object bool[] $Count
  $worst = 0.0
  $matched = 0
  for ($i = 0; $i -lt $Count; $i++) {
    $best = [double]::PositiveInfinity
    $bestIndex = -1
    for ($j = 0; $j -lt $Count; $j++) {
      if ($used[$j]) { continue }
      $d = Row-Distance -RowA $Left -RowB $Right -OffsetA ($i * $Length) -OffsetB ($j * $Length) -Length $Length
      if ($d -lt $best) { $best = $d; $bestIndex = $j }
    }
    if ($bestIndex -ge 0) {
      $used[$bestIndex] = $true
      $matched++
      if ($best -gt $worst) { $worst = $best }
    }
  }
  return @{ Matched = $matched; Worst = $worst }
}

function Max-Pairwise {
  param($Sets, [int]$Count, [int]$Length)
  $worst = 0.0
  for ($i = 0; $i -lt $Sets.Count; $i++) {
    for ($j = $i + 1; $j -lt $Sets.Count; $j++) {
      $d = (Compare-Centers -Left $Sets[$i] -Right $Sets[$j] -Count $Count -Length $Length).Worst
      if ($d -gt $worst) { $worst = $d }
    }
  }
  return $worst
}

function Min-DistanceToSubsequence {
  param([double[]]$Centre, [double[]]$Subsequences, [int]$Count, [int]$Length)
  $best = [double]::PositiveInfinity
  for ($s = 0; $s -lt $Count; $s++) {
    $d = Row-Distance -RowA $Centre -RowB $Subsequences -OffsetA 0 -OffsetB ($s * $Length) -Length $Length
    if ($d -lt $best) { $best = $d }
  }
  return $best
}

function ZScore-Block {
  param([double[]]$Values, [double[]]$Target, [int]$Offset, [int]$Length)
  $sum = 0.0
  for ($i = 0; $i -lt $Length; $i++) { $sum += $Values[$Offset + $i] }
  $mean = $sum / $Length
  $acc = 0.0
  for ($i = 0; $i -lt $Length; $i++) { $d = $Values[$Offset + $i] - $mean; $acc += $d * $d }
  $sd = [Math]::Sqrt($acc / ($Length - 1))
  for ($i = 0; $i -lt $Length; $i++) {
    $Target[$Offset + $i] = ($Values[$Offset + $i] - $mean) / $sd
  }
}

Write-Host ''
Write-Host '== Stage 4: Java reference implementation'

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
$java = Join-Path $JavaHome 'bin\java.exe'
$javac = Join-Path $JavaHome 'bin\javac.exe'
Check 'java present' (Test-Path $java)
Check 'javac present' (Test-Path $javac)
Check 'udf jar present' (Test-Path $UdfJar) -Detail $UdfJar
if ($script:Failed -gt 0) { Write-Host 'cannot continue'; exit 1 }

$source = Join-Path $PSScriptRoot '..\oracle\JavaOracle.java'
$classes = Join-Path $WorkDir 'classes'
New-Item -ItemType Directory -Path $classes -Force | Out-Null
& $javac -cp $UdfJar -d $classes $source 2>&1 | ForEach-Object { Write-Host "    $_" }
Check 'oracle compiles' ($LASTEXITCODE -eq 0)
if ($script:Failed -gt 0) { Write-Host 'cannot continue'; exit 1 }

# Strongly separated shapes, so the optimum is dominated by the data rather
# than by the initial assignment.
$csv = Join-Path $WorkDir 'oracle.csv'
$total = $K * $Copies * $L
$random = New-Object System.Random 20261030
$blocks = New-Object System.Collections.ArrayList
for ($shape = 0; $shape -lt $K; $shape++) {
  for ($copy = 0; $copy -lt $Copies; $copy++) {
    $row = New-Object double[] $L
    for ($i = 0; $i -lt $L; $i++) {
      $t = $i / [double]($L - 1)
      switch ($shape) {
        0 { $base = $t }
        1 { $base = 1 - $t }
        2 { $base = [Math]::Sin($t * 2 * 3.14159265358979) }
        default { $base = 0 }
      }
      $row[$i] = $base + ($random.NextDouble() - 0.5) * 0.04
    }
    [void]$blocks.Add($row)
  }
}
for ($i = $blocks.Count - 1; $i -gt 0; $i--) {
  $j = $random.Next($i + 1)
  $tmp = $blocks[$i]; $blocks[$i] = $blocks[$j]; $blocks[$j] = $tmp
}

$lines = New-Object System.Collections.Generic.List[string]
$ts = 0
foreach ($block in $blocks) {
  foreach ($value in $block) {
    $lines.Add(('{0},{1}' -f $ts, [Math]::Round($value, 6)))
    $ts++
  }
}
Set-Content -Path $csv -Value $lines -Encoding ASCII
Check 'oracle data written' ($ts -eq $total)

$flat = New-Object double[] $total
for ($i = 0; $i -lt $total; $i++) { $flat[$i] = [double]($lines[$i].Split(',')[1]) }
$normalized = New-Object double[] $total
$sequenceCount = [int]($total / $L)
for ($s = 0; $s -lt $sequenceCount; $s++) {
  ZScore-Block -Values $flat -Target $normalized -Offset ($s * $L) -Length $L
}

# --- Java reference, several runs ----------------------------------------
$javaSets = @()
for ($run = 0; $run -lt $JavaRuns; $run++) {
  $out = Join-Path $WorkDir ("java_k_{0}.txt" -f $run)
  & $java -cp "$classes;$UdfJar" JavaOracle k $csv $L $K $Edk 0.2 100 > $out 2>$null
  Check ("java run {0} executed" -f $run) ($LASTEXITCODE -eq 0)
  $values = Read-CenterFile $out
  Check ("java run {0} returned k*l" -f $run) ($values.Count -eq $K * $L) -Detail ("got {0}" -f $values.Count)
  $javaSets += , $values
}

# --- C++ side, several seeds ---------------------------------------------
$cppSets = @()
for ($seed = 1; $seed -le $CppSeeds; $seed++) {
  $db = Join-Path $WorkDir ("rdb_{0}" -f $seed)
  if (Test-Path $db) { Remove-Item $db -Recurse -Force }
  & $RocksDbCli import --db $db --ds 1 --csv $csv --k $K --l $L --page-points 1000000 --chunk-pages 1 --seed $seed 2>&1 | Out-Null
  Check ("cpp import seed {0}" -f $seed) ($LASTEXITCODE -eq 0)
  $out = Join-Path $WorkDir ("cpp_k_{0}.txt" -f $seed)
  & $RocksDbCli query --db $db --ds 1 --t0 0 --t1 ($total - 1) --method kshape_raw --out $out 2>$null | Out-Null
  Check ("cpp kshape_raw seed {0}" -f $seed) ($LASTEXITCODE -eq 0)
  $values = Read-CenterFile $out
  Check ("cpp seed {0} returned k*l" -f $seed) ($values.Count -eq $K * $L) -Detail ("got {0}" -f $values.Count)
  $cppSets += , $values
}

$javaSpread = Max-Pairwise -Sets $javaSets -Count $K -Length $L
$cppSpread = Max-Pairwise -Sets $cppSets -Count $K -Length $L
$agreement = [double]::PositiveInfinity
foreach ($cpp in $cppSets) {
  foreach ($ref in $javaSets) {
    $d = (Compare-Centers -Left $cpp -Right $ref -Count $K -Length $L).Worst
    if ($d -lt $agreement) { $agreement = $d }
  }
}

Write-Host ''
Write-Host '  K-Shape: C++ vs Java'
Write-Host ("    java run-to-run spread : {0:E3}" -f $javaSpread)
Write-Host ("    cpp  seed-to-seed spread: {0:E3}" -f $cppSpread)
Write-Host ("    cpp vs java (best pair) : {0:E3}" -f $agreement)
Write-Host ("    floating point tolerance: {0:E3}" -f $Tolerance)

Check 'cpp agrees with the reference within its own spread' `
      (($agreement -le [Math]::Max($javaSpread, $Tolerance)) -and ($agreement -ne [double]::PositiveInfinity)) `
      -Detail ("agreement={0:E3} javaSpread={1:E3}" -f $agreement, $javaSpread)
Check 'cpp is at least as stable as the reference' ($cppSpread -le [Math]::Max($javaSpread, $Tolerance)) `
      -Detail ("cppSpread={0:E3} javaSpread={1:E3}" -f $cppSpread, $javaSpread)

# --- Medoid-Shape structural check ---------------------------------------
$medoidOut = Join-Path $WorkDir 'cpp_m.txt'
$medoidDb = Join-Path $WorkDir 'rdb_1'
& $RocksDbCli query --db $medoidDb --ds 1 --t0 0 --t1 ($total - 1) --method mshape_raw --out $medoidOut 2>$null | Out-Null
Check 'cpp mshape_raw succeeded' ($LASTEXITCODE -eq 0)
$cppM = Read-CenterFile $medoidOut

$javaMOut = Join-Path $WorkDir 'java_m.txt'
& $java -cp "$classes;$UdfJar" JavaOracle m $csv $L $K $Edk 0.2 100 > $javaMOut 2>$null
Check 'java medoid oracle ran' ($LASTEXITCODE -eq 0)
$javaM = Read-CenterFile $javaMOut

Write-Host ''
Write-Host '  Medoid-Shape: structural check'
# A medoid is one of the input subsequences, never an average, so every
# returned row must coincide with a z-scored input subsequence.
$cppWorst = 0.0
for ($c = 0; $c -lt $K; $c++) {
  $slice = New-Object double[] $L
  for ($i = 0; $i -lt $L; $i++) { $slice[$i] = $cppM[$c * $L + $i] }
  $d = Min-DistanceToSubsequence -Centre $slice -Subsequences $normalized -Count $sequenceCount -Length $L
  if ($d -gt $cppWorst) { $cppWorst = $d }
}
Check 'cpp medoids are actual subsequences' ($cppWorst -lt 1e-9) -Detail ("worst={0:E3}" -f $cppWorst)

$javaWorst = 0.0
for ($c = 0; $c -lt $K; $c++) {
  $slice = New-Object double[] $L
  for ($i = 0; $i -lt $L; $i++) { $slice[$i] = $javaM[$c * $L + $i] }
  $d = Min-DistanceToSubsequence -Centre $slice -Subsequences $normalized -Count $sequenceCount -Length $L
  if ($d -gt $javaWorst) { $javaWorst = $d }
}
Check 'java medoids are actual subsequences' ($javaWorst -lt 1e-9) -Detail ("worst={0:E3}" -f $javaWorst)
Write-Host ("    cpp medoid vs java medoid (informational): {0:E3}" -f (Compare-Centers -Left $cppM -Right $javaM -Count $K -Length $L).Worst)

Write-Host ''
Write-Host '== Summary'
Write-Host ("  passed: {0}   failed: {1}" -f $script:Passed, $script:Failed)
if ($script:Failed -gt 0) {
  $script:Failures | ForEach-Object { Write-Host ("    - {0}" -f $_) }
  exit 1
}
Write-Host '  ORACLE CHECKS PASSED'
exit 0
