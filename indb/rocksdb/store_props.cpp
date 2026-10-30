//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Storage mode where the precomputed clustering metadata is written into the
// SST files themselves, through the table properties meta block.
//
// Import writes points only. Each page record is registered with the
// collector, and the database is flushed once per logical chunk, so one SST
// holds exactly one chunk and its meta block carries exactly that chunk's page
// records. Queries read those records back from the table properties.

#include <algorithm>
#include <cmath>
#include <limits>
#include <random>

#include "rocksdb/stats_props.h"
#include "rocksdb/store.h"

namespace indb {

void Store::ImportPointsToTableProperties(uint32_t ds, const Points& points, const Config& config,
                                          DatasetInfo* info) {
  rocksdb::ColumnFamilyHandle* cf_points = cf_for("points");
  rocksdb::WriteOptions write_options;

  const size_t total = points.size();
  size_t offset = 0;
  int64_t page_no = 0;
  int64_t chunk_no = 0;
  int64_t pages_in_chunk = 0;

  while (offset < total) {
    const size_t begin = offset;
    const size_t end = std::min(total, begin + static_cast<size_t>(config.page_points));

    rocksdb::WriteBatch batch;
    for (size_t i = begin; i < end; ++i) {
      std::string key;
      key.reserve(12);
      for (int shift = 24; shift >= 0; shift -= 8) {
        key.push_back(static_cast<char>((ds >> shift) & 0xFF));
      }
      const uint64_t encoded_ts = static_cast<uint64_t>(points.ts[i]) ^ 0x8000000000000000ULL;
      for (int shift = 56; shift >= 0; shift -= 8) {
        key.push_back(static_cast<char>((encoded_ts >> shift) & 0xFF));
      }
      batch.Put(cf_points, key,
                rocksdb::Slice(reinterpret_cast<const char*>(&points.values[i]), sizeof(double)));
    }
    const rocksdb::Status status = db_->Write(write_options, &batch);
    if (!status.ok()) throw AlgorithmError("point write failed: " + status.ToString());

    Points page;
    page.ts.assign(points.ts.begin() + begin, points.ts.begin() + end);
    page.values.assign(points.values.begin() + begin, points.values.begin() + end);

    const uint64_t page_seed =
        config.seed ^ (static_cast<uint64_t>(page_no + 1) * 0x9E3779B97F4A7C15ULL);
    const PageMeta meta = build_page_meta(page, config, page_seed);
    registry_->Put(ds, page_no, meta.t_start, meta.t_end, encode_meta(meta));

    ++page_no;
    ++pages_in_chunk;
    offset = end;

    // Seal the file at chunk boundaries so one SST holds one chunk.
    if (pages_in_chunk >= config.chunk_pages || offset >= total) {
      Flush();
      ++chunk_no;
      pages_in_chunk = 0;
    }
  }

  info->n_pages = page_no;
  info->n_chunks = chunk_no;
}

namespace {

// Point keys are dataset (u32 big endian) followed by the sign flipped
// timestamp (i64 big endian), so byte order matches numeric order.
std::string MakePointKey(uint32_t ds, int64_t ts) {
  std::string key;
  key.reserve(12);
  for (int shift = 24; shift >= 0; shift -= 8) {
    key.push_back(static_cast<char>((ds >> shift) & 0xFF));
  }
  const uint64_t encoded = static_cast<uint64_t>(ts) ^ 0x8000000000000000ULL;
  for (int shift = 56; shift >= 0; shift -= 8) {
    key.push_back(static_cast<char>((encoded >> shift) & 0xFF));
  }
  return key;
}

}  // namespace

std::vector<Store::SstGroup> Store::LoadPropertyGroups(uint32_t ds, int64_t t0, int64_t t1,
                                                       int64_t* bytes) const {
  std::vector<SstGroup> groups;
  if (bytes != nullptr) *bytes += 0;

  // Only the files that overlap the queried key range are visited, so this is
  // the on-disk metadata cost of one query rather than a full table scan.
  const std::string start = MakePointKey(ds, t0);
  const std::string limit = MakePointKey(ds, t1);
  const rocksdb::Range range{rocksdb::Slice(start), rocksdb::Slice(limit)};
  rocksdb::TablePropertiesCollection collection;
  const rocksdb::Status status = db_->GetPropertiesOfTablesInRange(
      const_cast<Store*>(this)->cf_for("points"), &range, 1, &collection);
  if (!status.ok()) throw AlgorithmError("cannot read table properties: " + status.ToString());

  for (const auto& entry : collection) {
    if (entry.second == nullptr) continue;
    const rocksdb::TableProperties& table = *entry.second;
    std::vector<PageMeta> pages;
    if (!DecodeTableProperties(table, &pages) || pages.empty()) continue;

    const auto found = table.user_collected_properties.find(kPageStatsProperty);
    if (bytes != nullptr && found != table.user_collected_properties.end()) {
      *bytes += static_cast<int64_t>(found->second.size());
    }

    SstGroup group;
    group.pages = std::move(pages);
    group.t_start = group.pages.front().t_start;
    group.t_end = group.pages.back().t_end;
    groups.push_back(std::move(group));
  }

  // The collection is an unordered map, and merging is order dependent because
  // centroids are updated as weighted averages and pages are attached to the
  // nearest running centroid. Sort by start time so the merge order matches the
  // order the page records were written in.
  std::sort(groups.begin(), groups.end(), [](const SstGroup& lhs, const SstGroup& rhs) {
    return lhs.t_start < rhs.t_start;
  });
  return groups;
}

QueryResult Store::QueryFromProperties(const QueryOptions& options, const Config& config,
                                       bool medoid) {
  QueryResult result;
  QueryStats stats;

  const std::vector<SstGroup> groups = LoadPropertyGroups(options.ds, options.t0, options.t1, &stats.meta_bytes_read);
  if (groups.empty()) {
    result.stats = stats;
    return result;
  }

  std::vector<PageMeta> units;

  auto consider_page = [&](const PageMeta& meta) {
    ++stats.pages_considered;
    const double ratio = overlap_ratio(meta, options.t0, options.t1);
    if (ratio <= 0.0) return;
    if (ratio >= config.theta_page) {
      units.push_back(meta);
      ++stats.pages_meta_used;
      return;
    }
    const Points raw = ReadRange(options.ds, meta.t_start, meta.t_end);
    units.push_back(correct_page(meta, raw, options.t0, options.t1, config, &stats));
    ++stats.pages_corrected;
  };

  if (options.level == Level::Chunk) {
    for (const SstGroup& group : groups) {
      PageMeta span = group.pages.front();
      span.t_start = group.t_start;
      span.t_end = group.t_end;
      const double ratio = overlap_ratio(span, options.t0, options.t1);
      if (ratio <= 0.0) continue;
      if (ratio >= config.theta_chunk) {
        // One file holds one chunk: derive the chunk record from its pages.
        units.push_back(build_chunk_meta(group.pages, config));
        ++stats.chunks_meta_used;
        continue;
      }
      ++stats.chunks_descended;
      for (const PageMeta& meta : group.pages) consider_page(meta);
    }
  } else {
    for (const SstGroup& group : groups) {
      for (const PageMeta& meta : group.pages) consider_page(meta);
    }
  }

  result.stats = stats;
  if (units.empty()) return result;

  const PageMeta merged = build_chunk_meta(units, config);
  result.stats.units_merged = static_cast<int64_t>(units.size());

  if (!medoid) {
    result.centers.resize(static_cast<size_t>(config.k));
    for (int c = 0; c < config.k; ++c) {
      Mat sum(config.l, config.l);
      for (int r = 0; r < config.l; ++r) {
        for (int q = 0; q < config.l; ++q) sum.at(r, q) = merged.sum_matrix(c, r, q);
      }
      Vec reference(static_cast<size_t>(config.l));
      for (int r = 0; r < config.l; ++r) reference[static_cast<size_t>(r)] = merged.centroid(c, r);
      result.centers[static_cast<size_t>(c)] = extract_shape(sum, reference);
    }
    return result;
  }

  // Medoid-Shape samples raw subsequences the same way the other storage mode
  // does; the candidates do not come from the metadata.
  std::mt19937 sampler(static_cast<uint32_t>(config.seed));
  std::vector<PageMeta> all_pages;
  for (const SstGroup& group : groups) {
    for (const PageMeta& meta : group.pages) all_pages.push_back(meta);
  }
  std::uniform_real_distribution<double> coin(0.0, 1.0);

  const int desired = std::max(config.k * 32, 128);
  std::vector<Vec> candidates;
  std::vector<double> weights;
  weights.reserve(all_pages.size());
  double total_weight = 0.0;
  for (const PageMeta& meta : all_pages) {
    const double weight = static_cast<double>(std::max<int64_t>(1, meta.count));
    weights.push_back(weight);
    total_weight += weight;
  }
  if (total_weight > 0.0) {
    std::uniform_real_distribution<double> pick(0.0, total_weight);
    for (int attempt = 0; attempt < desired * 8 && static_cast<int>(candidates.size()) < desired;
         ++attempt) {
      double value = pick(sampler);
      int index = 0;
      double accumulated = 0.0;
      for (size_t i = 0; i < all_pages.size(); ++i) {
        accumulated += weights[i];
        if (value <= accumulated) {
          index = static_cast<int>(i);
          break;
        }
      }
      if (coin(sampler) > config.r) continue;
      const Points raw = ReadRange(options.ds, all_pages[static_cast<size_t>(index)].t_start,
                                   all_pages[static_cast<size_t>(index)].t_end);
      stats.raw_points_read += static_cast<int64_t>(raw.size());
      const std::vector<Vec> sequences = grid_subsequences(raw, config.l);
      for (const Vec& seq : sequences) {
        if (static_cast<int>(candidates.size()) >= desired) break;
        candidates.push_back(seq);
      }
    }
  }

  result.stats = stats;
  if (candidates.empty()) return result;

  std::vector<Vec> normalized;
  normalized.reserve(candidates.size());
  for (const Vec& seq : candidates) {
    if (static_cast<int>(seq.size()) != config.l) continue;
    normalized.push_back(zscore(seq));
  }
  if (normalized.empty()) return result;

  Rng rng(config.seed);
  // Greedy maximum coverage, identical to the column family path.
  std::vector<Vec> coreset;
  const int n = static_cast<int>(normalized.size());
  while (static_cast<int>(coreset.size()) < config.k) {
    const int draw = std::max(1, static_cast<int>(static_cast<double>(n) * config.r));
    std::vector<int> pool(static_cast<size_t>(n));
    for (int i = 0; i < n; ++i) pool[static_cast<size_t>(i)] = i;
    std::vector<int> sampled;
    for (int i = 0; i < draw && !pool.empty(); ++i) {
      const int pos = rng.next_int(static_cast<int>(pool.size()));
      sampled.push_back(pool[static_cast<size_t>(pos)]);
      pool.erase(pool.begin() + pos);
    }
    double best_gain = -std::numeric_limits<double>::max();
    int best_index = -1;
    for (int candidate : sampled) {
      const double* probe = normalized[static_cast<size_t>(candidate)].data();
      double gain = 0.0;
      for (int c = 0; c < merged.edk; ++c) {
        if (merged.ed_counts[static_cast<size_t>(c)] == 0) continue;
        const double* centre = merged.ed_centroids.data() + static_cast<size_t>(c) * config.l;
        const Vec corr = ncc(probe, centre, config.l);
        double best = std::numeric_limits<double>::lowest();
        for (double v : corr) best = std::max(best, v);
        for (const Vec& chosen : coreset) {
          const Vec other = ncc(probe, chosen.data(), config.l);
          for (double v : other) best = std::max(best, v);
        }
        gain += best * static_cast<double>(merged.ed_counts[static_cast<size_t>(c)]);
      }
      if (gain > best_gain) {
        best_gain = gain;
        best_index = candidate;
      }
    }
    if (best_index < 0) break;
    coreset.push_back(normalized[static_cast<size_t>(best_index)]);
  }
  result.centers = coreset;
  return result;
}

}  // namespace indb
