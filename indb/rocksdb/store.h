//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// RocksDB adapter: storage layout, metadata persistence and the four query
// paths. Algorithms live in indb/core and are shared with the PostgreSQL
// implementation.

#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "indb/core.h"
#include "rocksdb/db.h"
#include "rocksdb/options.h"

namespace indb {

class PageMetaRegistry;

// Where the precomputed clustering metadata lives.
//
// kColumnFamily    - a dedicated column family, one key per page. This mirrors
//                    the IoTDB implementation, which keeps page statistics in
//                    a separate region of the file.
// kTableProperties - inside the SST that holds the data points, in the meta
//                    block RocksDB writes for table properties. This is the
//                    layout the paper describes.
enum class MetaStorage : int32_t {
  kColumnFamily = 0,
  kTableProperties = 1,
};

// Persisted description of one series. The parameters are stored with the
// data so a query with different k/l/edk is rejected instead of silently
// misreading metadata produced under other settings.
struct DatasetInfo {
  uint32_t ds = 0;
  int32_t version = 2;
  MetaStorage storage = MetaStorage::kColumnFamily;
  Config config;
  int64_t first_ts = 0;
  int64_t last_ts = 0;
  int64_t n_points = 0;
  int64_t n_pages = 0;
  int64_t n_chunks = 0;
};

// Options that apply to a single query.
struct QueryOptions {
  uint32_t ds = 0;
  int64_t t0 = 0;
  int64_t t1 = 0;
  // Zero means "use the value recorded with the dataset", so a query cannot
  // silently run with parameters that did not produce the stored metadata.
  int k = 0;
  int l = 0;
  int edk = 0;
  double r = 0.2;
  int page_points = 0;   // 0 => use the value recorded in the catalog
  int chunk_pages = 0;   // 0 => use the value recorded in the catalog
  Level level = Level::Page;
};

class Store {
 public:
  // Opens an existing database. All column families must already exist.
  static std::unique_ptr<Store> Open(const std::string& path);
  // Creates the database and its column families. Fails if it already exists.
  static std::unique_ptr<Store> Create(const std::string& path);
  ~Store();

  Store(const Store&) = delete;
  Store& operator=(const Store&) = delete;

  // Imports points from a two column CSV (ts,value). A single column file is
  // accepted as well, in which case the timestamp is the row number. Returns
  // the number of points written.
  int64_t ImportCsv(uint32_t ds, const std::string& csv_path, const Config& config,
                    bool skip_header, MetaStorage storage);

  // Persists a synthetic series directly, used by tests.
  int64_t ImportPoints(uint32_t ds, const Points& points, const Config& config,
                       MetaStorage storage);

  void Flush();

  DatasetInfo Describe(uint32_t ds) const;
  std::vector<DatasetInfo> ListDatasets() const;

  // The four query entry points.
  QueryResult KShapeRaw(const QueryOptions& options);
  QueryResult MShapeRaw(const QueryOptions& options);
  QueryResult KShape(const QueryOptions& options);
  QueryResult MShape(const QueryOptions& options);

 private:
  Store();

  rocksdb::ColumnFamilyHandle* cf_for(const std::string& name);

  void WriteBatchOfPoints(uint32_t ds, const Points& points, size_t* offset,
                          const Config& config, DatasetInfo* info);

  // Writes points only, registering each page record for the properties
  // collector, and flushes once per chunk so one SST holds one chunk.
  void ImportPointsToTableProperties(uint32_t ds, const Points& points, const Config& config,
                                     DatasetInfo* info);

  Points ReadRange(uint32_t ds, int64_t t0, int64_t t1) const;
  Points ReadPage(uint32_t ds, int64_t page_no) const;

  std::vector<int64_t> PagesCovering(uint32_t ds, int64_t t0, int64_t t1) const;
  std::vector<int64_t> ChunksCovering(uint32_t ds, int64_t t0, int64_t t1) const;

  PageMeta ReadPageMeta(uint32_t ds, int64_t page_no, int64_t* bytes) const;
  struct ChunkRecord {
    PageMeta meta;
    int64_t page_from = 0;
    int64_t page_to = 0;
  };
  ChunkRecord ReadChunkMeta(uint32_t ds, int64_t chunk_no, int64_t* bytes) const;

  // Page records read out of the SST meta blocks, grouped by file.
  struct SstGroup {
    std::vector<PageMeta> pages;
    int64_t t_start = 0;
    int64_t t_end = 0;
  };
  std::vector<SstGroup> LoadPropertyGroups(uint32_t ds, int64_t t0, int64_t t1, int64_t* bytes) const;

  // Query path used when the dataset stores its metadata in table properties.
  QueryResult QueryFromProperties(const QueryOptions& options, const Config& config, bool medoid);

  Config ResolveConfig(const QueryOptions& options, const DatasetInfo& info) const;

  std::unique_ptr<rocksdb::DB> db_;
  std::vector<rocksdb::ColumnFamilyHandle*> handles_;
  std::shared_ptr<PageMetaRegistry> registry_;
};

}  // namespace indb
