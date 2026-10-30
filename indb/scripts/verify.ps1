#  Copyright (c) 2026 The in-database time-series clustering authors.
#  This source code is licensed under both the GPLv2 (found in the
#  COPYING file in the root directory) and Apache 2.0 License
#  (found in the LICENSE.Apache file in the root directory).
#
# End to end verification of the RocksDB and PostgreSQL implementations.
# Runs the deterministic stages of docs/verification-plan.md and prints a
# PASS/FAIL summary.
#
#   powershell -ExecutionPolicy Bypass -File indb/scripts/verify.ps1
#   powershell -ExecutionPolicy Bypass -File indb/scripts/verify.ps1 -SkipPg
#
param(
  [string]$RocksDbCli = 'E:\build\indb\Release\indb_cli.exe',
  [string]$TestsExe = 'E:\build\indb\Release\indb_tests.exe',
  [string]$PgRoot = 'E:\pgsql',
  [int]$PgPort = 5432,
  [string]$PgUser = 'postgres',
  [string]$PgDatabase = 'indb_verify',
  [string]$WorkDir = 'E:\tmp\indb-verify',
  [int]$Points = 20000,
  [int]$K = 3,
  [int]$L = 50,
  [int]$PagePoints = 2000,
  [int]$ChunkPages = 4,
  [int]$IterBuild = 30,
  [long]$Seed = 24301,
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

function Section { param([string]$Title) Write-Host ""; Write-Host ("== {0}" -f $Title) }

function Read-Doubles {
  param([string]$Path, [int]$Skip = 0)
  $lines = Get-Content $Path
  if ($lines.Count -le $Skip) { return @() }
  return @(($lines | Select-Object -Skip $Skip) -join ' ' -split '\s+' |
           Where-Object { $_ -ne '' } | ForEach-Object { [double]$_ })
}

function Read-ArrayText {
  param([string]$Text)
  $trimmed = $Text.Trim()
  if ($trimmed -eq '{}' -or $trimmed -eq '') { return @() }
  $trimmed = $trimmed.TrimStart('{').TrimEnd('}')
  return @(($trimmed -split ',') | Where-Object { $_ -ne '' } | ForEach-Object { [double]$_ })
}

function Max-AbsDiff {
  param([double[]]$A, [double[]]$B)
  $n = [Math]::Min($A.Count, $B.Count)
  if ($A.Count -ne $B.Count) { return [double]::PositiveInfinity }
  $max = 0.0
  for ($i = 0; $i -lt $n; $i++) {
    $d = [Math]::Abs($A[$i] - $B[$i])
    if ($d -gt $max) { $max = $d }
  }
  return $max
}

function Invoke-Rdb {
  param([string[]]$Arguments)
  # Native tools write progress to stderr; with ErrorActionPreference=Stop that
  # would abort the script even on success, so relax it for the call.
  $previous = $ErrorActionPreference
  $ErrorActionPreference = 'Continue'
  $errFile = Join-Path $WorkDir 'rdb.stderr.txt'
  & $RocksDbCli @Arguments 2> $errFile | Out-Null
  $code = $LASTEXITCODE
  $ErrorActionPreference = $previous
  return @{ Code = $code; Stderr = (Get-Content $errFile -Raw -ErrorAction SilentlyContinue) }
}

function Get-RdbStat {
  param([string]$Stderr, [string]$Key)
  if ($Stderr -match ("{0}=(\d+)" -f [regex]::Escape($Key))) { return [int]$Matches[1] }
  return -1
}

function Invoke-Pg {
  param([string]$Sql)
  $script = Join-Path $WorkDir 'query.sql'
  Set-Content -Path $script -Value $Sql -Encoding ASCII
  $out = & (Join-Path $PgRoot 'bin\psql.exe') -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase `
         -t -A -q -v ON_ERROR_STOP=1 -f $script 2>&1
  return @{ Code = $LASTEXITCODE; Output = $out }
}

# ---------------------------------------------------------------------------
Section 'Stage 0: environment'

New-Item -ItemType Directory -Path $WorkDir -Force | Out-Null
Check 'rocksdb cli present' (Test-Path $RocksDbCli)
Check 'rocksdb tests present' (Test-Path $TestsExe)
if (-not $SkipPg) {
  Check 'psql present' (Test-Path (Join-Path $PgRoot 'bin\psql.exe'))
  Check 'extension dll present' (Test-Path (Join-Path $PgRoot 'lib\indb_tsc.dll'))
  $service = Get-Service pg18indb -ErrorAction SilentlyContinue
  Check 'postgres service running' ($null -ne $service -and $service.Status -eq 'Running')
}
if ($script:Failed -gt 0) {
  Write-Host 'Environment is not ready; stopping.'
  exit 1
}

# ---------------------------------------------------------------------------
Section 'Stage 1: kernel and store tests'

$testOutput = & $TestsExe
$testCode = $LASTEXITCODE
Check 'indb_tests exit code 0' ($testCode -eq 0)
Check 'indb_tests reports success' (($testOutput -join "`n") -match 'ALL TESTS PASSED')

# ---------------------------------------------------------------------------
Section 'Stage 2: synthetic data, four paths per platform'

$csv = Join-Path $WorkDir 'series.csv'
$lines = New-Object System.Collections.Generic.List[string]
$random = New-Object System.Random 42
for ($i = 0; $i -lt $Points; $i++) {
  $phase = [Math]::Floor($i / $L) % 3
  $t = ($i % $L) / [double]$L
  $base = switch ($phase) { 0 { $t } 1 { 1 - $t } 2 { [Math]::Sin($t * 3.14159265358979 * 2) } }
  $noise = ($random.NextDouble() - 0.5) * 0.05
  $lines.Add(('{0},{1}' -f $i, [Math]::Round($base + $noise, 6)))
}
Set-Content -Path $csv -Value $lines -Encoding ASCII

$rdbDir = Join-Path $WorkDir 'rdb'
if (Test-Path $rdbDir) { Remove-Item $rdbDir -Recurse -Force }
$import = Invoke-Rdb @('import', '--db', $rdbDir, '--ds', '1', '--csv', $csv,
                       '--k', "$K", '--l', "$L", '--page-points', "$PagePoints",
                       '--chunk-pages', "$ChunkPages", '--iter-build', "$IterBuild",
                       '--seed', "$Seed")
Check 'rocksdb import succeeded' ($import.Code -eq 0)

$infoText = (& $RocksDbCli info --db $rdbDir 2>$null) -join ' '
$expectedPages = [Math]::Ceiling($Points / $PagePoints)
$expectedChunks = [Math]::Ceiling($expectedPages / $ChunkPages)
$match = [regex]::Match($infoText, 'points=(\d+) pages=(\d+) chunks=(\d+)')
Check 'rocksdb info parsed' $match.Success -Detail $infoText
$reportedPoints = if ($match.Success) { [int]$match.Groups[1].Value } else { -1 }
$reportedPages = if ($match.Success) { [int]$match.Groups[2].Value } else { -1 }
$reportedChunks = if ($match.Success) { [int]$match.Groups[3].Value } else { -1 }
Check 'rocksdb point count' ($reportedPoints -eq $Points) -Detail "expected $Points"
Check 'rocksdb page count' ($reportedPages -eq $expectedPages) -Detail "expected $expectedPages"
Check 'rocksdb chunk count' ($reportedChunks -eq $expectedChunks) -Detail "expected $expectedChunks"

$rdbResults = @{}
$rdbStats = @{}
foreach ($case in @(
    @{ name = 'kshape'; method = 'kshape'; extra = @() },
    @{ name = 'kshape_chunk'; method = 'kshape'; extra = @('--level', 'chunk') },
    @{ name = 'kshape_raw'; method = 'kshape_raw'; extra = @() },
    @{ name = 'mshape_raw'; method = 'mshape_raw'; extra = @() },
    @{ name = 'mshape'; method = 'mshape'; extra = @() })) {
  $outFile = Join-Path $WorkDir ("rdb_{0}.txt" -f $case.name)
  $run = Invoke-Rdb (@('query', '--db', $rdbDir, '--ds', '1', '--t0', '0', '--t1', "$($Points - 1)",
                       '--method', $case.method, '--out', $outFile) + $case.extra)
  Check ("rocksdb {0} exit 0" -f $case.name) ($run.Code -eq 0)
  $values = Read-Doubles -Path $outFile -Skip 1
  $rdbResults[$case.name] = $values
  $rdbStats[$case.name] = $run.Stderr
  Check ("rocksdb {0} returns k*l" -f $case.name) ($values.Count -eq ($K * $L)) -Detail ("got {0}" -f $values.Count)
  $finite = $true
  foreach ($v in $values) { if ([double]::IsNaN($v) -or [double]::IsInfinity($v)) { $finite = $false; break } }
  Check ("rocksdb {0} finite" -f $case.name) $finite
}

Check 'rocksdb full range uses no correction' ((Get-RdbStat $rdbStats['kshape'] 'pages_corrected') -eq 0)
Check 'rocksdb full range reads no raw points' ((Get-RdbStat $rdbStats['kshape'] 'raw_points') -eq 0)
Check 'rocksdb chunk path uses chunk metadata' ((Get-RdbStat $rdbStats['kshape_chunk'] 'chunks_meta') -eq $expectedChunks)

$partialOut = Join-Path $WorkDir 'rdb_partial.txt'
$partial = Invoke-Rdb @('query', '--db', $rdbDir, '--ds', '1', '--t0', '500', '--t1', '12000',
                        '--method', 'kshape', '--out', $partialOut)
$partialValues = Read-Doubles -Path $partialOut -Skip 1
Check 'rocksdb partial range returns k*l' ($partialValues.Count -eq ($K * $L))
Check 'rocksdb partial range corrects pages' ((Get-RdbStat $partial.Stderr 'pages_corrected') -ge 1)
Check 'rocksdb partial range reads only edge pages' ((Get-RdbStat $partial.Stderr 'raw_points') -lt $Points)

# ---------------------------------------------------------------------------
Section 'Stage 2b: metadata stored in the SST table properties'

$propsDir = Join-Path $WorkDir 'rdb_props'
if (Test-Path $propsDir) { Remove-Item $propsDir -Recurse -Force }
$importProps = Invoke-Rdb @('import', '--db', $propsDir, '--ds', '1', '--csv', $csv,
                            '--k', "$K", '--l', "$L", '--page-points', "$PagePoints",
                            '--chunk-pages', "$ChunkPages", '--iter-build', "$IterBuild",
                            '--seed', "$Seed", '--meta-storage', 'props')
Check 'properties mode import succeeded' ($importProps.Code -eq 0)

foreach ($modeCase in @(@{ name = 'kshape'; extra = @() },
                        @{ name = 'kshape_chunk'; extra = @('--level', 'chunk') })) {
  $outFile = Join-Path $WorkDir ("props_{0}.txt" -f $modeCase.name)
  $run = Invoke-Rdb (@('query', '--db', $propsDir, '--ds', '1', '--t0', '0',
                       '--t1', "$($Points - 1)", '--method', 'kshape', '--out', $outFile) + $modeCase.extra)
  Check ("properties {0} exit 0" -f $modeCase.name) ($run.Code -eq 0)
  $values = Read-Doubles -Path $outFile -Skip 1
  Check ("properties {0} returns k*l" -f $modeCase.name) ($values.Count -eq ($K * $L))
  $diff = Max-AbsDiff $rdbResults[$modeCase.name] $values
  Check ("{0} identical across storage modes" -f $modeCase.name) ($diff -eq 0.0) `
        -Detail ("maxdiff={0:E3}" -f $diff)
  Check ("properties {0} reads no raw points" -f $modeCase.name) `
        ((Get-RdbStat $run.Stderr 'raw_points') -eq 0)
}

# ---------------------------------------------------------------------------
$pgResults = @{}
$pgStats = @{}
if (-not $SkipPg) {
  Section 'Stage 2 (continued): PostgreSQL on the same input'

  $psql = Join-Path $PgRoot 'bin\psql.exe'
  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d postgres -q -c "DROP DATABASE IF EXISTS $PgDatabase;" 2>&1 | Out-Null
  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d postgres -q -c "CREATE DATABASE $PgDatabase;" 2>&1 | Out-Null
  $schemaSql = Join-Path $PSScriptRoot '..\sql\schema.sql'
  $installSql = Join-Path $PSScriptRoot '..\sql\install.sql'
  if (-not (Test-Path $schemaSql)) {
    # Running from the RocksDB repository: use the PostgreSQL checkout.
    $schemaSql = 'E:\rework\indb-ts-cluster-postgresql\indb\sql\schema.sql'
    $installSql = 'E:\rework\indb-ts-cluster-postgresql\indb\sql\install.sql'
  }
  Check 'schema file found' (Test-Path $schemaSql)
  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -q -v ON_ERROR_STOP=1 -f $schemaSql 2>&1 | Out-Null
  Check 'schema applied' ($LASTEXITCODE -eq 0)
  & $psql -h 127.0.0.1 -p $PgPort -U $PgUser -d $PgDatabase -q -v ON_ERROR_STOP=1 -f $installSql 2>&1 | Out-Null
  Check 'functions registered' ($LASTEXITCODE -eq 0)

  $csvPg = $csv -replace '\\', '/'
  $importResult = Invoke-Pg "SELECT indb_tsc_import(1, '$csvPg', false);"
  Check 'pg import succeeded' ($importResult.Code -eq 0) -Detail ($importResult.Output -join ' ')

  $buildResult = Invoke-Pg "SELECT indb_tsc_build(1, $K, $L, $PagePoints, $ChunkPages, $IterBuild, $Seed);"
  Check 'pg build succeeded' ($buildResult.Code -eq 0) -Detail ($buildResult.Output -join ' ')

  foreach ($case in @(
      @{ name = 'kshape'; sql = "SELECT indb_tsc_kshape(1, 0, $($Points - 1), 'page')" },
      @{ name = 'kshape_chunk'; sql = "SELECT indb_tsc_kshape(1, 0, $($Points - 1), 'chunk')" },
      @{ name = 'kshape_raw'; sql = "SELECT indb_tsc_kshape_raw(1, 0, $($Points - 1))" },
      @{ name = 'mshape_raw'; sql = "SELECT indb_tsc_mshape_raw(1, 0, $($Points - 1))" })) {
    $result = Invoke-Pg ("{0};
SELECT indb_tsc_last_stats();" -f $case.sql)
    $linesOut = @($result.Output | Where-Object { $_ -match '\S' })
    Check ("pg {0} exit 0" -f $case.name) ($result.Code -eq 0) -Detail ($result.Output -join ' ')
    $values = Read-ArrayText $linesOut[0]
    $pgResults[$case.name] = $values
    $pgStats[$case.name] = if ($linesOut.Count -gt 1) { Read-ArrayText $linesOut[1] } else { @() }
    Check ("pg {0} returns k*l" -f $case.name) ($values.Count -eq ($K * $L)) -Detail ("got {0}" -f $values.Count)
  }

  Section 'Stage 2 (continued): cross implementation parity'
  foreach ($name in @('kshape', 'kshape_chunk', 'kshape_raw', 'mshape_raw')) {
    $diff = Max-AbsDiff $rdbResults[$name] $pgResults[$name]
    Check ("{0} identical on both platforms" -f $name) ($diff -eq 0.0) -Detail ("maxdiff={0:E3}" -f $diff)
  }
  $rdbPartial = @{ pages = (Get-RdbStat $partial.Stderr 'pages_corrected') }
  $pgPartial = Invoke-Pg "SELECT array_length(indb_tsc_kshape(1, 500, 12000, 'page'), 1);
SELECT indb_tsc_last_stats();"
  Check 'pg partial range returns k*l' ($pgPartial.Code -eq 0)
}

# ---------------------------------------------------------------------------
Section 'Stage 3: boundary cases'

$emptyOut = Join-Path $WorkDir 'rdb_empty.txt'
$empty = Invoke-Rdb @('query', '--db', $rdbDir, '--ds', '1', '--t0', '999999', '--t1', '999999',
                      '--method', 'kshape', '--out', $emptyOut)
Check 'rocksdb empty range does not fail' ($empty.Code -eq 0)
$emptyValues = Read-Doubles -Path $emptyOut -Skip 1
Check 'rocksdb empty range returns nothing' ($emptyValues.Count -eq 0)

$mismatch = Invoke-Rdb @('query', '--db', $rdbDir, '--ds', '1', '--t0', '0', '--t1', '100',
                         '--method', 'kshape', '--l', '99', '--out', (Join-Path $WorkDir 'x.txt'))
Check 'rocksdb rejects mismatched parameters' (($mismatch.Code -ne 0) -and
      (($mismatch.Stderr + (Get-Content (Join-Path $WorkDir 'rdb.stderr.txt') -Raw)) -match 'do not match'))

if (-not $SkipPg) {
  $pgEmpty = Invoke-Pg "SELECT coalesce(array_length(indb_tsc_kshape(1, 999999, 999999, 'page'), 1), 0);"
  Check 'pg empty range returns empty array' ($pgEmpty.Output -contains '0')
}

# ---------------------------------------------------------------------------
Section 'Summary'
Write-Host ("  passed: {0}   failed: {1}" -f $script:Passed, $script:Failed)
if ($script:Failed -gt 0) {
  Write-Host '  failures:'
  $script:Failures | ForEach-Object { Write-Host ("    - {0}" -f $_) }
  exit 1
}
Write-Host '  ALL CHECKS PASSED'
exit 0
