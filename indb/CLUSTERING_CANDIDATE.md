<!--
Licensed to the Apache Software Foundation (ASF) under one or more
contributor license agreements. See the NOTICE file distributed with
this work for additional information regarding copyright ownership.
The ASF licenses this file to you under the Apache License, Version 2.0
(the "License"); you may not use this file except in compliance with
the License. You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
-->

# Clustering implementation candidate - 2026-11-08

This snapshot is a candidate for a new round of experiments. Persisted Chunk
metadata and coverage-threshold selection remain the core method. Page-M and
Chunk-M share the platform's Page subtraction correction when usable Page
metadata falls below the Page coverage threshold. Unusable or conflicting
metadata still takes the existing raw recovery path.

The original Page correction removes wholly outside subsequences, so a
subsequence crossing a query edge can retain a contribution. This is not a
strict query-intersection refit. K queries retain their existing boundary path.
Historical measurements remain results of their recorded implementation;
they should not be relabeled as measurements of this candidate.

## RocksDB add-on changes

- Properties queries first inspect a lightweight directory over existing v1/v2
  payloads, then fully decode only the selected Page or Chunk statistics.
  The owning table properties stay alive for the lifetime of the byte views.
- M sampling uses lightweight page ranges and counts; it does not force Page
  clustering arrays to be decoded. Sampling weights, RNG and candidate order
  are retained.
- Chunk-M reuses Page correction; its transient merge conserves M weights
  independently of invalidated K memberships.
- A weighted Chunk center can cancel to zero. Only non-finite Chunk merge
  distances fall back to the existing normalized-correlation assignment
  distance, preventing lost weights. Page merge arithmetic remains unchanged.

The SST format is unchanged. Properties blobs are still fetched as a whole;
reduced decoding/allocation is not a claim of reduced physical I/O.
The optional `INDB_DECODE_DIAGNOSTICS` macro is absent from normal builds.
Decoding counters and phase timings were checked once, outside the experiment
pipeline, and are not a new mandatory experiment check.

## Validation

The standalone `indb/CMakeLists.txt` builds the add-on against an existing
RocksDB library. `ctest --output-on-failure` runs the core/store tests and the
focused Page-M correction regression, including the repeated-shape case that
previously lost Chunk weights. Source-built artifacts also passed synthetic
queries with full/partial ranges, g=1/g=4, dynamic/coarse paths and short tails.
The upstream RocksDB engine is unchanged; this validation covers the add-on.
