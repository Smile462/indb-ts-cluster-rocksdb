# In-database time-series clustering on RocksDB

Implementation of the in-database K-Shape / Medoid-Shape clustering pipeline on
RocksDB, ported from the IoTDB reference implementation
(`Smile462/iotdb-kshape-mshape-clustering`).

The repository root is an unmodified RocksDB v11.8.1 source tree. Everything in
this directory (`indb/`) is the add-on; it links against an already built
`rocksdb.lib` and does not modify any RocksDB file.

## What is implemented

Four query paths, each available at page and chunk granularity:

| Path | Input | Work at query time |
|---|---|---|
| `kshape_raw` | raw points of the range | full K-Shape from scratch |
| `mshape_raw` | raw points of the range | full Medoid-Shape from scratch |
| `kshape` | persisted page or chunk metadata (edge pages re-read) | merge, correct boundaries, extract eigenvectors |
| `mshape` | persisted metadata plus sampled subsequences | merge, greedy medoid selection |

The two `*_raw` methods correspond to the IoTDB UDFs (`kshapeudf`,
`kshapemudf`); the two metadata methods correspond to `lsmKShape` /
`lsmMShape` with `level=page|chunk`.

## Layout

```
indb/
  core/include/indb/core.h    parameters, metadata, kernel API
  core/src/math.cpp           z-score, FFT, NCC, SBD, eigensolver, K-Shape, K-Means
  core/src/meta.cpp           page/chunk metadata construction, merge, encoding
  core/src/query.cpp          the four query paths, boundary correction, medoids
  rocksdb/store.h,store.cpp   column families, key encoding, import, query glue
  tools/main.cpp              CLI: import / info / query
  tests/test_core.cpp         kernel and end-to-end tests
```

`indb/core` is storage agnostic and is the authoritative copy. The PostgreSQL
repository vendors an identical snapshot of it.

## Build

Prerequisites: MSVC 2022 (C++20), CMake >= 3.16, a built `rocksdb.lib`.

```powershell
cmake -S indb -B E:/build/indb -G "Visual Studio 17 2022" -A x64 `
      -DROCKSDB_LIBRARY=E:/build/rocksdb/Release/rocksdb.lib
cmake --build E:/build/indb --config Release -- /m:24
E:/build/indb/Release/indb_tests.exe
```

RocksDB itself is built from the repository root:

```powershell
cmake -S . -B E:/build/rocksdb -G "Visual Studio 17 2022" -A x64 `
      -DCMAKE_BUILD_TYPE=Release -DWITH_TESTS=OFF -DWITH_TOOLS=OFF `
      -DWITH_SNAPPY=OFF -DWITH_LZ4=OFF -DWITH_ZSTD=OFF -DWITH_ZLIB=OFF
cmake --build E:/build/rocksdb --config Release --target rocksdb -- /m:24
```

## Storage layout

| Column family | Key | Value |
|---|---|---|
| `points` | `ds(u32 BE) + ts(i64 BE, sign flipped)` | `double` |
| `pagemeta` | `ds(u32) + page_no(u64)` | encoded `PageMeta` |
| `chunkmeta` | `ds(u32) + chunk_no(u64)` | encoded `PageMeta` (chunk level) |
| `pageindex` | `ds(u32) + page_no(u64)` | `t_start(i64), t_end(i64)` |
| `chunkindex` | `ds(u32) + chunk_no(u64)` | `page_from, page_to, t_start, t_end` |
| `catalog` | `ds(u32)` | parameters, counts, time range |

Keys are big endian so RocksDB's byte-wise comparator reproduces numeric order.

A **logical page** is `page_points` consecutive points of one series; a
**logical chunk** is `chunk_pages` consecutive pages. Neither is tied to an SST
file, so compaction cannot invalidate them.

Per page the pipeline stores exactly what the IoTDB implementation stores in
the page header: per cluster summed outer products (`k*l*l` doubles),
centroids, deltas, membership labels, the approximate `edK` model, and the
head/tail partial subsequences that let adjacent pages be stitched together.

## Storage modes

The precomputed clustering metadata can live in two places, selected at import
time with `--meta-storage`:

| Mode | Where the metadata lives | CLI |
|---|---|---|
| `cf` (default) | a dedicated column family, one key per page | `--meta-storage cf` |
| `props` | inside the SST that holds the data points, in the table properties meta block | `--meta-storage props` |

`props` is the layout described in the paper: a `TablePropertiesCollector`
observes every key as the table is built, and at `Finish()` it stores the page
records for the time range that file covers under one user collected property
(`indb.page_stats`). The records therefore sit in the same file as the points
they describe. Import flushes once per logical chunk, so one SST carries
exactly one chunk. No RocksDB source change is involved: the mechanism is the
public table properties extension point.

Queries in `props` mode call `GetPropertiesOfTablesInRange`, which visits only
the files overlapping the requested key range, so the reported
`meta_bytes_read` is the metadata actually touched by that query.

Both modes run the same kernel on the same page records and produce bit
identical centres; `indb_tests` asserts this. Measured on the Air series
(714962 points, k=3, l=166, 10240 points per page, 8 pages per chunk):

| | `cf` | `props` |
|---|---:|---:|
| import | 43.7 s | 43.9 s |
| database size | 65.5 MB | 59.5 MB |
| `kshape` page, open | 0.007 s | 0.019 s |
| `kshape` page, query | 0.232 s | 0.230 s |
| `kshape` chunk, open | 0.007 s | 0.018 s |
| `kshape` chunk, query | 0.215 s | 0.210 s |

`props` is smaller because chunk aggregates are derived from the page records
at query time instead of being stored a second time.

Open and query time are reported separately on purpose. Opening a database can
touch table metadata, so folding it into one number would hide where the time
went. The `query` figures are the time from the first call to the last, on an
already opened database; the CLI prints `open`, `query_total`, `query_mean` and
`total` for every run.

The split is a reporting boundary, not a claim about where the I/O happens: the
operating system page cache and the size of the metadata both influence it, so
the timings alone cannot prove whether the properties block was read while
opening. Settling that needs RocksDB I/O counters, not millisecond arithmetic.

Layout is frozen: automatic compaction is disabled at creation and queries open
the database read only. No file is ever rewritten, so the mapping from files to
chunks stays valid. If a file that overlaps a query nevertheless carries no page
records, the query fails instead of aggregating over the gap, and the loader
also rejects overlapping or non contiguous page ranges.
## Query semantics

- Subsequences are aligned to the absolute grid `[i*l, (i+1)*l)`.
- A page is used directly when its overlap with the range reaches
  `theta_page` (0.8). Otherwise its raw points are read and the subsequences
  that fall entirely outside the range are subtracted from the summed
  matrices.
- At chunk level a chunk is used directly when its overlap reaches
  `theta_chunk` (0.8); otherwise the query descends to the pages it contains.
- Page ranges are located with binary search over `pageindex`, so a query reads
  `O(log pages)` index records plus the metadata it actually needs.

Every query reports counters on stderr: pages considered, pages served from
metadata, pages corrected, chunks used, chunks descended, units merged, raw
points read, metadata bytes read.

## CLI

```powershell
indb_cli import --db E:/tmp/db --ds 1 --csv air.csv --k 3 --l 166 `
                --page-points 10240 --chunk-pages 8
indb_cli info   --db E:/tmp/db
indb_cli query  --db E:/tmp/db --ds 1 --t0 0 --t1 714961 --method kshape
indb_cli query  --db E:/tmp/db --ds 1 --t0 0 --t1 714961 --method kshape --level chunk
indb_cli query  --db E:/tmp/db --ds 1 --t0 0 --t1 714961 --method mshape
indb_cli query  --db E:/tmp/db --ds 1 --t0 0 --t1 714961 --method kshape_raw
```

Centres are written to stdout as `k l` followed by `k` rows of `l` values.

## Current limitations

- Import is single threaded.
- One series per dataset id; the metadata format is per series.
- Automatic compaction is left at RocksDB defaults. Chunk metadata is derived
  from page metadata and is not tied to SST files, so compaction does not
  invalidate it. Rebuilding after deletes or updates is not implemented; the
  dataset is treated as static.
- RocksDB is built without compression for now.

See `docs/algorithm-notes.md` for the deliberate deviations from the IoTDB
reference.
