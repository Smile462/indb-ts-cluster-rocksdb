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
//
// The layout is frozen (no compaction, read only opens), so the mapping between
// files and chunks stays valid for the lifetime of the database. The loader
// still validates that mapping on every query rather than assuming it.

#include <algorithm>
#include <limits>
#include <random>

#include "rocksdb/stats_props.h"
#include "rocksdb/store.h"

namespace indb {
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
      batch.Put(cf_points, MakePointKey(ds, points.ts[i]),
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

    // Seal the file at chunk boundaries so one SST holds one chunk. Flush
    // throws on failure, so a file that failed to be written is never counted.
    if (pages_in_chunk >= config.chunk_pages || offset >= total) {
      Flush();
      ++chunk_no;
      pages_in_chunk = 0;
    }
  }

  info->n_pages = page_no;
  info->n_chunks = chunk_no;
}

std::vector<Store::SstGroup> Store::LoadPropertyGroups(uint32_t ds, int64_t t0, int64_t t1,
                                                       int64_t* bytes) const {
  std::vector<SstGroup> groups;

  // Only the files that overlap the queried key range are visited, so this is
  // the metadata cost of one query rather than a scan of the whole database.
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

    const auto found = table.user_collected_properties.find(kPageStatsProperty);
    if (found == table.user_collected_properties.end()) {
      // A file covering the queried range without page records means the
      // metadata was lost, for example because the file was rewritten.
      // Dropping it silently would return a partial answer that looks valid.
      throw AlgorithmError("table " + entry.first +
                           " overlaps the query range but carries no page statistics; the "
                           "dataset was probably rewritten");
    }

    std::vector<PageMeta> pages;
    std::string error;
    if (!DecodePayload(found->second, &pages, &error)) {
      throw AlgorithmError("cannot decode page statistics of table " + entry.first + ": " + error);
    }
    if (pages.empty()) {
      throw AlgorithmError("table " + entry.first + " has an empty page statistics payload");
    }
    if (bytes != nullptr) *bytes += static_cast<int64_t>(found->second.size());

    SstGroup group;
    group.pages = std::move(pages);
    group.t_start = group.pages.front().t_start;
    group.t_end = group.pages.back().t_end;
    groups.push_back(std::move(group));
  }

  // The collection is an unordered map, and merging is order dependent because
  // centroids are updated as weighted averages and pages attach to the nearest
  // running centroid. Sort by start time so the merge order matches the order
  // the page records were written in.
  std::sort(groups.begin(), groups.end(), [](const SstGroup& lhs, const SstGroup& rhs) {
    return lhs.t_start < rhs.t_start;
  });

  // Validate that the page records form a gap free, non overlapping sequence.
  // A gap means a file's records are missing and must not be aggregated over.
  bool first = true;
  int64_t previous_end = 0;
  for (const SstGroup& group : groups) {
    for (const PageMeta& page : group.pages) {
      if (!first) {
        if (page.t_start <= previous_end) {
          throw AlgorithmError("overlapping page records at time " + std::to_string(page.t_start));
        }
        if (page.t_start > previous_end + 1) {
          throw AlgorithmError("page records are not contiguous: missing range after " +
                               std::to_string(previous_end));
        }
      }
      previous_end = page.t_end;
      first = false;
    }
  }
  return groups;
}

QueryResult Store::QueryFromProperties(const QueryOptions& options, const Config& config,
                                       bool medoid) {
  QueryResult result;
  QueryStats stats;

  const std::vector<SstGroup> groups =
      LoadPropertyGroups(options.ds, options.t0, options.t1, &stats.metadata_payload_bytes);

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
      // With the frozen layout one file holds exactly one chunk, so the chunk
      // record is derived from the pages of that file.
      PageMeta span = group.pages.front();
      span.t_start = group.t_start;
      span.t_end = group.t_end;
      const double ratio = overlap_ratio(span, options.t0, options.t1);
      if (ratio <= 0.0) continue;
      if (ratio >= config.theta_chunk) {
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

  if (units.empty()) {
    result.stats = stats;
    return result;
  }
  stats.units_merged = static_cast<int64_t>(units.size());

  if (!medoid) {
    result = kshape_centers(units, config);
  } else {
    std::vector<PageMeta> all_pages;
    for (const SstGroup& group : groups) {
      for (const PageMeta& meta : group.pages) all_pages.push_back(meta);
    }
    // SampleCandidates only considers pages that overlap the query range, so a
    // narrow query cannot read points from the rest of the file.
    const std::vector<Vec> candidates = SampleCandidates(
        all_pages, options.t0, options.t1, config,
        [this, options](const PageMeta& meta) {
          return ReadRange(options.ds, meta.t_start, meta.t_end);
        },
        &stats);
    if (candidates.empty()) {
      result.stats = stats;
      return result;
    }
    result = medoid_centers(units, candidates, config);
  }

  // Assign the counters last so the helpers above cannot reset them.
  result.stats = stats;
  return result;
}

}  // namespace indb
