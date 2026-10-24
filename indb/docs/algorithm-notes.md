# Algorithm notes: what is ported and what deviates

Reference: `Smile462/iotdb-kshape-mshape-clustering` (IoTDB 0.13.3 plus the
clustering add-on). The audit of that change set is in the workspace document
`docs/01-iotdb-change-audit.md`.

## Ported faithfully

- Grid alignment: subsequence `i` covers `[i*l, (i+1)*l)` in absolute time.
- Page split into `head_extra` (stored at page relative offsets), complete
  subsequences, and `tail_extra` (right aligned). Adjacent pages satisfy
  `len(head) + len(tail) = l`, so element-wise addition reconstructs the
  boundary subsequence.
- z-score with sample standard deviation (`l - 1` divisor).
- FFT size `2 ^ bit_length(2l - 1)`, NCC index layout, and the two distinct
  normalisation conventions:
  - `sbd()` divides by `|x||y|` twice, matching `MathUtils._sbd` and
    `KShape._sbd` (used for centroid comparison and delta accumulation);
  - the K-Shape assignment step divides once, matching `_ncc(...).max()`.
- K-Shape: initial labels drawn from `[0, k]` (label `k` is out of range and
  those points are unassigned during the first round), centroid extraction from
  `P S P` with `P = I - J/(l-1)`, sign resolved against the whole population,
  centroid z-scored.
- K-Means for the approximate layer: squared L2 distances, K-Means++
  initialisation, empty clusters filled with a random point.
- Medoid-Shape: `edK = k + 2`, sampling rate `r`, greedy maximum coverage
  `max sum_i max_j ncc(ed_centroid_i, coreset_j) * count_i`.
- Metadata field set: summed matrices, centroids, deltas, membership, the
  approximate model, and both partial subsequences.

## Deliberate deviations

These are places where the reference implementation is internally inconsistent
or unsafe. Each one changes results relative to IoTDB, mostly on pages that
straddle a query boundary.

1. **Membership array length.** The reference allocates `idx` and `ed_idx` with
   a fixed 1000 entries. Here they are sized to the actual number of
   subsequences, so a page capacity above roughly `1000 * l` cannot overflow.

2. **Boundary correction uses z-scored values.** When a page is only partially
   covered, the reference subtracts the outer product of the *raw* subsequence
   from a matrix that was accumulated from *z-scored* subsequences. This port
   z-scores before subtracting, so the subtraction actually cancels the stored
   contribution.

3. **Centroids are rebuilt from the corrected matrix.** The reference recomputes
   the cluster centroids from the matrix captured *before* the subtraction,
   leaving the centroid inconsistent with the matrix it is paired with. This
   port uses the corrected matrix.

4. **Medoid boundary correction uses L2.** The reference looks up the nearest
   approximate centroid with SBD, but then updates that centroid as a Euclidean
   running mean over `ed_centroids`. This port uses L2 for both the lookup and
   the update.

5. **Pages without a complete subsequence.** If a page holds `l` or more points
   but still yields no grid aligned subsequence (it starts far enough off grid),
   the reference throws an array index error. This port stores an empty
   metadata record that keeps only the time range.

6. **Parameters are explicit and validated.** The reference reads `k` and `l`
   from static fields initialised from global configuration at class load time.
   Here parameters travel with the query, are recorded in the catalog, and a
   query whose `k` / `l` / `edK` disagree with the stored metadata is rejected.

7. **Deterministic randomness.** The reference uses unseeded `java.util.Random`
   in several places, so repeated runs differ. Here every page uses a seed
   derived from the dataset seed and the page number, which makes a rebuild
   reproduce the same metadata. Raw paths use the dataset seed.

8. **Empty cluster resolution in K-Means** compares values rather than array
   identities, with a bounded retry, instead of relying on Java reference
   equality.

## Known consequences

- Results on fully covered ranges should track IoTDB closely; results on
  partially covered ranges will differ at the boundary pages because of
  deviations 2 to 4.
- The raw paths (`kshape_raw`, `mshape_raw`) are unaffected by deviations 2 to
  4 and should match the reference UDF output up to random initialisation.
- `iter_build` is 30 and `iter_raw` is 100, matching the reference split between
  the page builder and the UDFs. Both are configurable.
