#  Copyright (c) 2026 The in-database time-series clustering authors.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
#
# Stage 5 of docs/verification-plan.md: run the real Air dataset through both
# platforms and check that the metadata paths stay correct at real scale.
#
#   powershell -ExecutionPolicy Bypass -File indb/scripts/verify-realdata.ps1
#
param(
  [string]$SourceDir = 'E:\实验\overlap\air\0',
  [string]$Csv = '',
  [string]$RocksDbCli = 'E:\build\indb\Release\indb_cli.exe',
  [string]$PgRoot = 'E:\pgsql',
  [int]$PgPort = 5432,
  [string]$PgUser = 'postgres',
  [string]$PgDatabase = 'indb_air',
  [string]$WorkDir = 'E:\tmp\indb-verify',
  [int]$K = 3,
  [int]$L = 166,
  [int]$PagePoints = 10240,
  [int]$ChunkPages = 8,
  [int]$IterBuild = 30,
  [long]$Seed = 24301,
  [double]$MinSpeedup = 10.0,
  [switch]$SkipPg
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

function Read-RdbFile {
  param([string]$Path)
  $lines = @(Get-Content $Path)
  if ($lines.Count -lt 1) { return @() }
  return @(($lines | Select-Object -Skip 1) -join ' ' -split '\s+' |
           Where-Object { $_ -ne '' } | ForEach-Object { [double]$_ })
}

function Read-PgFile {
  param([string]$Path)
  $text = (Get-Content $Path -Raw).Trim()
  if ($text -eq '') { return @() }
  $text = $text.TrimStart('{').TrimEnd('}')
  if ($text -eq '') { return @() }
  return @($text -split ',' | Where-Object { $_ -ne '' } | ForEach-Object { [double]$_ })
}

function Read-ArrayText {
  param([string]$Text)
  $trimmed = $Text.Trim()
  if ($trimmed -eq '' -or $trimmed -eq '{}') { return @() }
  $trimmed = $trimmed.TrimStart('{').TrimEnd('}')
  return @($trimmed -split ',' | Where-Object { $_ -ne '' } | ForEach-Object { [double]$_ })
}

function Max-Diff {
  param([double[]]$A, [double[]]$B)
  if ($A.Count -ne $B.Count) { return [double]::PositiveInfinity }
  $max = 0.0
  for ($i = 0; $i -lt $A.Count; $i++) {
    $d = [Math]::Abs($A[$i] - $B[$i])
    if ($d -gt $max) { $max = $d }
  }
  return $max
}

function Get-Stat {
  param([string]$Text, [string]$Key)
  if ($Text -match ("{0}=(\d+)" -f [regex]::Escape($Key))) { return [int]$Matches[1] }
  return -1
}

Write-Host ''
Write-Host '== Stage 5: real dataset'

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null

# --- build the flat series -------------------------------------------------
if ($Csv -eq '') {
  $Csv = Join-Path $WorkDir 'air.csv'
  if (-not (Test-Path $SourceDir)) {
    Write-Host ("  source directory not found: {0}" -f $SourceDir)
    exit 1
  }
  $files = Get-ChildItem $SourceDir -File -Filter '*.csv' |
           Sort-Object { [int]($_.BaseName) }
  Check 'source files found' ($files.Count -gt 0) -Detail $SourceDir
  if ($script:Failed -gt 0) { exit 1 }

  $invariant = [Globalization.CultureInfo]::InvariantCulture
  $writer = New-Object System.IO.StreamWriter($Csv, $false, [Text.Encoding]::ASCII)
  $rows = 0
  foreach ($file in $files) {
    foreach ($line in [IO.File]::ReadAllLines($file.FullName)) {
      $comma = $line.IndexOf(',')
      if ($comma -le 0) { continue }
      $stamp = 0L
      if (-not [long]::TryParse($line.Substring(0, $comma).Trim(), [ref]$stamp)) { continue }
      $value = 0.0
      if (-not [double]::TryParse($line.Substring($comma + 1).Trim(),
                                   [Globalization.NumberStyles]::Float, $invariant, [ref]$value)) { continue }
      $writer.WriteLine(('{0},{1}' -f $stamp, $value.ToString('R', $invariant)))
      $rows++
    }
  }
  $writer.Close()
  Check 'series built' ($rows -gt 0) -Detail ("{0} points" -f $rows)
} else {
  $rows = (Get-Content $Csv | Measure-Object -Line).Lines
  Check 'series supplied' (Test-Path $Csv) -Detail $Csv
}

$expectedPages = [int][Math]::Ceiling($rows / $PagePoints)
$expectedChunks = [int][Math]::Ceiling($expectedPages / $ChunkPages)
Write-Host ("    points={0} expected pages={1} chunks={2}" -f $rows, $expectedPages, $expectedChunks)

# --- RocksDB ---------------------------------------------------------------
Write-Host ''
Write-Host '  RocksDB'
$db = Join-Path $WorkDir 'air_rdb'
if (Test-Path $db) { Remove-Item $db -Recurse -Force }
$watch = [Diagnostics.Stopwatch]::StartNew()
& $RocksDbCli import --db $db --ds 1 --csv $Csv --k $K --l $L --page-points $PagePoints `
                     --chunk-pages $ChunkPages --iter-build $IterBuild --seed $Seed 2>&1 | Out-Null
$watch.Stop()
$importSeconds = $watch.Elapsed.TotalSeconds
Check 'rocksdb import succeeded' ($LASTEXITCODE -eq 0) -Detail ("{0:N1} s" -f $importSeconds)
Write-Host ("    import: {0:N1} s" -f $importSeconds)

$infoText = (& $RocksDbCli info --db $db 2>$null) -join ' '
$match = [regex]::Match($infoText, 'points=(\d+) pages=(\d+) chunks=(\d+)')
Check 'rocksdb info parsed' $match.Success -Detail $infoText
if ($match.Success) {
  Check 'rocksdb point count' ([int]$match.Groups[1].Value -eq $rows)
  Check 'rocksdb page count' ([int]$match.Groups[2].Value -eq $expectedPages)
  Check 'rocksdb chunk count' ([int]$match.Groups[3].Value -eq $expectedChunks)
}

$rdbValues = @{}
$rdbStats = @{}
$rdbSeconds = @{}
foreach ($case in @(
    @{ name = 'kshape'; extra = @('--method', 'kshape') },
    @{ name = 'kshape_chunk'; extra = @('--method', 'kshape', '--level', 'chunk') },
    @{ name = 'kshape_raw'; extra = @('--method', 'kshape_raw') },
    @{ name = 'mshape_raw'; extra = @('--method', 'mshape_raw') })) {
  $out = Join-Path $WorkDir ("air_{0}.txt" -f $case.name)
  $err = Join-Path $WorkDir 'air.stderr.txt'
  $watch = [Diagnostics.Stopwatch]::StartNew()
  & $RocksDbCli query --db $db --ds 1 --t0 0 --t1 ($rows - 1) @($case.extra) --out $out 2> $err | Out-Null
  $watch.Stop()
  $rdbSeconds[$case.name] = $watch.Elapsed.TotalSeconds
  $rdbValues[$case.name] = Read-RdbFile $out
  $rdbStats[$case.name] = (Get-Content $err -Raw -ErrorAction SilentlyContinue)
  Check ("rocksdb {0} returns k*l" -f $case.name) ($rdbValues[$case.name].Count -eq $K * $L) `
        -Detail ("got {0}" -f $rdbValues[$case.name].Count)
  Write-Host ("    {0,-13} {1,8:N3} s" -f $case.name, $rdbSeconds[$case.name])
}

Check 'rocksdb full range needs no correction' ((Get-Stat $rdbStats['kshape'] 'pages_corrected') -eq 0)
Check 'rocksdb metadata path reads no raw points' ((Get-Stat $rdbStats['kshape'] 'raw_points') -eq 0)
Check 'rocksdb chunk path uses chunk metadata' ((Get-Stat $rdbStats['kshape_chunk'] 'chunks_meta') -eq $expectedChunks)

$dbSizeMb = [Math]::Round(((Get-ChildItem $db -Recurse -File | Measure-Object Length -Sum).Sum / 1MB), 1)
Write-Host ("    database size: {0} MB" -f $dbSizeMb)

# --- PostgreSQL ------------------------------------------------------------
if (-not $SkipPg) {
  Write-Host ''
  Write-Host '  PostgreSQL'
  $psql = Join-Path $PgRoot 'bin\psql.exe'
  $sqlDir = Join-Path $PSScriptRoot '..\sql'
  if (-not (Test-Path (Join-Path $sqlDir 'schema.sql'))) {
    $sqlDir = 'E:\rework\indb-ts-cluster-postgresql\indb\sql'
  }
  Check 'postgres schema available' (Test-Path (Join-Path $sqlDir 'schema.sql')) -Detail $sqlDir

  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d postgres -q -c "DROP DATABASE IF EXISTS $PgDatabase;" 2>&1 | Out-Null
  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d postgres -q -c "CREATE DATABASE $PgDatabase;" 2>&1 | Out-Null
  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -q -v ON_ERROR_STOP=1 -f (Join-Path $sqlDir 'schema.sql') 2>&1 | Out-Null
  Check 'postgres schema applied' ($LASTEXITCODE -eq 0)
  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -q -v ON_ERROR_STOP=1 -f (Join-Path $sqlDir 'install.sql') 2>&1 | Out-Null
  Check 'postgres functions registered' ($LASTEXITCODE -eq 0)

  $csvPg = $Csv -replace '\\', '/'
  $watch = [Diagnostics.Stopwatch]::StartNew()
  $importOut = & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -t -A `
                       -c "SELECT indb_tsc_import(1, '$csvPg', false);" 2>&1
  $watch.Stop()
  $pgImportSeconds = $watch.Elapsed.TotalSeconds
  Check 'postgres import succeeded' ("$importOut".Trim() -eq "$rows") -Detail ("$importOut")
  Write-Host ("    import: {0:N1} s" -f $pgImportSeconds)

  $watch = [Diagnostics.Stopwatch]::StartNew()
  $buildOut = & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -t -A `
                      -c "SELECT indb_tsc_build(1, $K, $L, $PagePoints, $ChunkPages, $IterBuild, $Seed);" 2>&1
  $watch.Stop()
  $pgBuildSeconds = $watch.Elapsed.TotalSeconds
  Check 'postgres build succeeded' ("$buildOut".Trim() -eq "$rows") -Detail ("$buildOut")
  Write-Host ("    build: {0:N1} s" -f $pgBuildSeconds)

  $counts = & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -t -A `
                    -c "SELECT n_points, n_pages, n_chunks FROM ts_dataset WHERE ds = 1;" 2>&1
  $parts = ("$counts".Trim() -split '\|')
  Check 'postgres page count' ([int]$parts[1] -eq $expectedPages) -Detail ("$counts")
  Check 'postgres chunk count' ([int]$parts[2] -eq $expectedChunks) -Detail ("$counts")

  $pgValues = @{}
  $pgStats = @{}
  foreach ($case in @(
      @{ name = 'kshape'; sql = "SELECT indb_tsc_kshape(1, 0, $($rows - 1), 'page')" },
      @{ name = 'kshape_chunk'; sql = "SELECT indb_tsc_kshape(1, 0, $($rows - 1), 'chunk')" },
      @{ name = 'kshape_raw'; sql = "SELECT indb_tsc_kshape_raw(1, 0, $($rows - 1))" },
      @{ name = 'mshape_raw'; sql = "SELECT indb_tsc_mshape_raw(1, 0, $($rows - 1))" })) {
    $script = Join-Path $WorkDir 'air_query.sql'
    Set-Content -Path $script -Value ("\timing on`n{0};`nSELECT indb_tsc_last_stats();`n" -f $case.sql) -Encoding ASCII
    $output = & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -t -A -f $script 2>&1
    $lines = @($output | Where-Object { $_ -match '^\{' })
    $elapsed = -1.0
    foreach ($line in $output) {
      if ($line -match 'Time:\s+([\d.]+)\s+ms') { $elapsed = [double]$Matches[1]; break }
    }
    $values = if ($lines.Count -gt 0) { Read-ArrayText $lines[0] } else { @() }
    $pgValues[$case.name] = $values
    $pgStats[$case.name] = if ($lines.Count -gt 1) { $lines[1] } else { '' }
    Check ("postgres {0} returns k*l" -f $case.name) ($values.Count -eq $K * $L) -Detail ("got {0}" -f $values.Count)
    Write-Host ("    {0,-13} {1,8:N0} ms" -f $case.name, $elapsed)
  }

  Write-Host ''
  Write-Host '  Cross implementation parity'
  foreach ($name in @('kshape', 'kshape_chunk', 'kshape_raw', 'mshape_raw')) {
    $diff = Max-Diff $rdbValues[$name] $pgValues[$name]
    Check ("{0} identical on both platforms" -f $name) ($diff -eq 0.0) -Detail ("maxdiff={0:E3}" -f $diff)
  }
}

# --- speed sanity ----------------------------------------------------------
Write-Host ''
Write-Host '  Metadata path versus from-scratch'
$speedup = 0.0
if ($rdbSeconds['kshape'] -gt 0) { $speedup = $rdbSeconds['kshape_raw'] / $rdbSeconds['kshape'] }
Write-Host ("    rocksdb kshape_raw / kshape = {0:N1}x" -f $speedup)
Check 'metadata path is materially faster than from scratch' ($speedup -ge $MinSpeedup) `
      -Detail ("speedup {0:N1}x, required {1:N1}x" -f $speedup, $MinSpeedup)

Write-Host ''
Write-Host '== Summary'
Write-Host ("  passed: {0}   failed: {1}" -f $script:Passed, $script:Failed)
if ($script:Failed -gt 0) {
  $script:Failures | ForEach-Object { Write-Host ("    - {0}" -f $_) }
  exit 1
}
Write-Host '  REAL DATA CHECKS PASSED'
exit 0
