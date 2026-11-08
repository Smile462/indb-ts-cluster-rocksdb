#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cmath>
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

#ifdef INDB_DECODE_DIAGNOSTICS
// One-off verification build only. Ordinary experiment builds omit this probe.
struct DecodeProbe {
  using Clock = std::chrono::steady_clock;
  Clock::time_point started = Clock::now();
  int64_t pages = 0, chunks = 0, page_bytes = 0, chunk_bytes = 0;
  double directory = 0, decode = 0, selection = 0, candidate = 0, solver = 0;
  ~DecodeProbe() {
    const double total = std::chrono::duration<double>(Clock::now() - started).count();
    std::fprintf(stderr, "JOURNAL_DECODE:{\"page_decodes\":%lld,\"chunk_decodes\":%lld,\"page_decode_bytes\":%lld,\"chunk_decode_bytes\":%lld,\"directory_seconds\":%.9f,\"full_decode_seconds\":%.9f,\"selection_including_decode_seconds\":%.9f,\"candidate_seconds\":%.9f,\"solver_seconds\":%.9f,\"properties_query_seconds\":%.9f}\n",
        static_cast<long long>(pages), static_cast<long long>(chunks), static_cast<long long>(page_bytes), static_cast<long long>(chunk_bytes), directory, decode, selection, candidate, solver, total);
  }
};
#endif

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

  const char* mode=std::getenv("JOURNAL_STORAGE_STATE");
  const bool raw_only=mode && std::string(mode)=="S0";
  const bool page_only=mode && std::string(mode)=="SP";
  double page_seconds=0,chunk_seconds=0,flush_seconds=0;
  const size_t total = points.size();
  size_t offset = 0;
  int64_t page_no = 0;
  int64_t chunk_no = 0;
  int64_t pages_in_chunk = 0;
  std::vector<PageMeta> chunk_pages;
  chunk_pages.reserve(static_cast<size_t>(config.chunk_pages));

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

    if(!raw_only) {
    Points page;
    page.ts.assign(points.ts.begin() + begin, points.ts.begin() + end);
    page.values.assign(points.values.begin() + begin, points.values.begin() + end);

    const uint64_t page_seed =
        config.seed ^ (static_cast<uint64_t>(page_no + 1) * 0x9E3779B97F4A7C15ULL);
    const auto page_begin=std::chrono::steady_clock::now();
    const PageMeta meta = build_page_meta(page, config, page_seed);
    page_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-page_begin).count();
    registry_->PutPage(ds, page_no, meta.t_start, meta.t_end, encode_meta(meta));
    if(!page_only && meta.count<config.l) {
      const auto short_begin=std::chrono::steady_clock::now();
      chunk_pages.push_back(build_page_meta(page,chunk_config(config),page_seed));
      chunk_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-short_begin).count();
    } else chunk_pages.push_back(meta);

    }

    ++page_no;
    ++pages_in_chunk;
    offset = end;

    // Seal the file at chunk boundaries so one SST holds one chunk. Flush
    // throws on failure, so a file that failed to be written is never counted.
    if (pages_in_chunk >= config.chunk_pages || offset >= total) {
      // The chunk record is stored in the same properties block as the pages it
      // summarises, so a chunk level query reads it instead of recomputing it.
      if(!raw_only && !page_only) {
      const auto chunk_begin=std::chrono::steady_clock::now();
      const PageMeta chunk = build_chunk_meta(chunk_pages, chunk_config(config));
      chunk_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-chunk_begin).count();
      registry_->PutChunk(ds, chunk.t_start, chunk.t_end, encode_meta(chunk));
      }
      const auto flush_begin=std::chrono::steady_clock::now();Flush();
      flush_seconds+=std::chrono::duration<double>(std::chrono::steady_clock::now()-flush_begin).count();
      ++chunk_no;
      pages_in_chunk = 0;
      chunk_pages.clear();
    }
  }

  std::fprintf(stderr,"JOURNAL_BUILD page_seconds=%.9f chunk_seconds=%.9f flush_seconds=%.9f\n",page_seconds,chunk_seconds,flush_seconds);
  info->n_pages = raw_only ? 0 : page_no;
  info->n_chunks = (raw_only || page_only) ? 0 : chunk_no;
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

    PayloadView view;
    std::string error;
    if (!IndexPayload(found->second, &view, &error)) {
      throw AlgorithmError("cannot index page statistics of table " + entry.first + ": " + error);
    }
    if (view.pages.empty()) {
      throw AlgorithmError("table " + entry.first + " has an empty page statistics payload");
    }
    if (bytes != nullptr) *bytes += static_cast<int64_t>(found->second.size());

    SstGroup group;
    group.properties = entry.second;
    group.pages = std::move(view.pages);
    group.chunk = view.chunk;
    group.has_chunk = view.has_chunk;
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
    for (const MetaRecordView& page : group.pages) {
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
  Config page_correction_config = config;
  page_correction_config.medoid_page_correction = medoid;

#ifdef INDB_DECODE_DIAGNOSTICS
  DecodeProbe probe;
  const auto directory_started = DecodeProbe::Clock::now();
#endif
  const std::vector<SstGroup> groups =
      LoadPropertyGroups(options.ds, options.t0, options.t1, &stats.metadata_payload_bytes);
#ifdef INDB_DECODE_DIAGNOSTICS
  probe.directory = std::chrono::duration<double>(DecodeProbe::Clock::now() - directory_started).count();
  const auto selection_started = DecodeProbe::Clock::now();
#endif

  auto decode_record = [&](const MetaRecordView& view, bool chunk) {
#ifdef INDB_DECODE_DIAGNOSTICS
    const auto started = DecodeProbe::Clock::now();
#endif
    PageMeta meta = decode_meta(std::string(view.encoded));
#ifdef INDB_DECODE_DIAGNOSTICS
    probe.decode += std::chrono::duration<double>(DecodeProbe::Clock::now() - started).count();
    if (chunk) { ++probe.chunks; probe.chunk_bytes += view.encoded.size(); }
    else { ++probe.pages; probe.page_bytes += view.encoded.size(); }
#endif
    return meta;
  };
  auto range_ratio = [&](int64_t start, int64_t end) {
    const int64_t left = std::max(start, options.t0), right = std::min(end, options.t1);
    return left > right ? 0.0 : static_cast<double>(right - left + 1) / static_cast<double>(end - start + 1);
  };

  std::vector<PageMeta> units;

  auto short_chunk_page = [&](const PageMeta& meta) {
    if(meta.count>=config.l || meta.grid_fragments)return meta;
    const Points raw=ReadRange(options.ds,meta.t_start,meta.t_end);
    stats.raw_points_read+=raw.size();
    return build_page_meta(raw,chunk_config(config),config.seed);
  };
  auto dynamic_chunk = [&](const std::vector<MetaRecordView>& stored) {
    std::vector<PageMeta> pages;
    pages.reserve(stored.size());
    for (const auto& view : stored) pages.push_back(decode_record(view, false));
    for(auto& page:pages)if(page.count<config.l)page=short_chunk_page(page);
    return build_chunk_meta(pages,chunk_config(config));
  };
  auto consider_page = [&](const MetaRecordView& view) {
    ++stats.pages_considered;
    const double ratio = range_ratio(view.t_start, view.t_end);
    if (ratio <= 0.0) return;
    const PageMeta meta = decode_record(view, false);
    if (ratio >= config.theta_page) {
      units.push_back(config.chunk_path && meta.count<config.l?short_chunk_page(meta):meta);
      ++stats.pages_meta_used;
      return;
    }
    const Points raw = ReadRange(options.ds, meta.t_start, meta.t_end);
    units.push_back(correct_page(meta, raw, options.t0, options.t1, page_correction_config, &stats));
    ++stats.pages_corrected;
  };

  if (options.level == Level::Chunk) {
    for (const SstGroup& group : groups) {
      // With the frozen layout one file holds exactly one chunk, so the chunk
      // record is derived from the pages of that file.
      const double ratio = range_ratio(group.t_start, group.t_end);
      if (ratio <= 0.0) continue;
      if (ratio >= config.theta_chunk || options.ablation == "coarse") {
        if(options.ablation!="dynamic" && !group.has_chunk)throw AlgorithmError("Persisted chunk record missing");
        units.push_back(options.ablation == "dynamic" ? dynamic_chunk(group.pages) : decode_record(group.chunk, true));
        ++stats.chunks_meta_used;
        continue;
      }
      ++stats.chunks_descended;
      for (const MetaRecordView& meta : group.pages) consider_page(meta);
    }
  } else {
    for (const SstGroup& group : groups) {
      for (const MetaRecordView& meta : group.pages) consider_page(meta);
    }
  }

#ifdef INDB_DECODE_DIAGNOSTICS
  probe.selection = std::chrono::duration<double>(DecodeProbe::Clock::now() - selection_started).count();
#endif

  if (units.empty()) {
    result.stats = stats;
    return result;
  }
  stats.units_merged = static_cast<int64_t>(units.size());

  if (!medoid) {
#ifdef INDB_DECODE_DIAGNOSTICS
    const auto started = DecodeProbe::Clock::now();
#endif
    result = kshape_centers(units, config);
#ifdef INDB_DECODE_DIAGNOSTICS
    probe.solver = std::chrono::duration<double>(DecodeProbe::Clock::now() - started).count();
#endif
  } else {
    std::vector<CandidatePageRange> all_pages;
    for (const SstGroup& group : groups) {
      for (const MetaRecordView& meta : group.pages)
        all_pages.push_back({meta.t_start, meta.t_end, meta.count, all_pages.size()});
    }
    // Preserve candidate order/weights using only the lightweight page ranges;
    // whole-page extraction keeps the existing candidate sampling policy.
#ifdef INDB_DECODE_DIAGNOSTICS
    const auto candidate_started = DecodeProbe::Clock::now();
#endif
    const std::vector<Vec> candidates = SampleCandidateRanges(
        all_pages, options.t0, options.t1, config,
        [this, options](const CandidatePageRange& meta) {
          return ReadRange(options.ds, meta.t_start, meta.t_end);
        },
        &stats);
#ifdef INDB_DECODE_DIAGNOSTICS
    probe.candidate = std::chrono::duration<double>(DecodeProbe::Clock::now() - candidate_started).count();
#endif
    stats.candidate_count=static_cast<int64_t>(candidates.size());
    if (candidates.empty()) {
      result.stats = stats;
      return result;
    }
#ifdef INDB_DECODE_DIAGNOSTICS
    const auto solver_started = DecodeProbe::Clock::now();
#endif
    result = medoid_centers(units, candidates, config);
#ifdef INDB_DECODE_DIAGNOSTICS
    probe.solver = std::chrono::duration<double>(DecodeProbe::Clock::now() - solver_started).count();
#endif
  }

  stats.represented_subsequences=result.stats.represented_subsequences;
  // Assign the counters last so the helpers above cannot reset them.
  result.stats = stats;
  return result;
}

std::string Store::JournalStorageInfo(uint32_t ds) const {
  rocksdb::TablePropertiesCollection collection;
  auto status=db_->GetPropertiesOfAllTables(const_cast<Store*>(this)->cf_for("points"),&collection);
  if(!status.ok())throw AlgorithmError(status.ToString());
  int64_t page_bytes=0,chunk_bytes=0,index_bytes=0,blob_bytes=0;
  for(const auto& item:collection){
    index_bytes+=item.second->index_size;
    auto f=item.second->user_collected_properties.find(kPageStatsProperty);if(f==item.second->user_collected_properties.end())continue;
    std::vector<PageMeta> pages;PageMeta chunk;bool has=false;std::string error;
    if(!DecodePayload(f->second,&pages,&chunk,&has,&error))throw AlgorithmError(error);
    blob_bytes+=f->second.size();for(const auto& page:pages)page_bytes+=encode_meta(page).size();
    if(has)chunk_bytes+=encode_meta(chunk).size();
  }
  return "{\"page_payload_bytes\":"+std::to_string(page_bytes)+",\"chunk_payload_bytes\":"+std::to_string(chunk_bytes)+",\"points_table_index_bytes\":"+std::to_string(index_bytes)+",\"properties_blob_bytes\":"+std::to_string(blob_bytes)+"}";
}

}  // namespace indb
