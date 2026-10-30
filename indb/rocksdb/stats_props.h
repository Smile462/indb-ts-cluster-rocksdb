//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Persists clustering metadata inside the SST files themselves, using the meta
// block RocksDB natively writes for table properties.
//
// A TablePropertiesCollector observes every key as the table is built. It
// records the time range the file covers, then at Finish() asks the registry for
// the page records and the chunk record that belong to that range, and stores
// both under a single user collected property. The payload therefore lives in
// the properties meta block of the very file that holds the corresponding data
// points.
//
// This is the "natively supported meta block" layout described in the paper: no
// RocksDB source changes are involved.

#pragma once

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "indb/core.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/table_properties.h"

namespace indb {

// Name of the user collected property that carries the clustering records.
extern const char* const kPageStatsProperty;

// Current payload layout version. Version 2 adds the chunk record.
constexpr uint8_t kPayloadVersion = 2;

struct PageMetaEntry {
  int64_t page_no = 0;
  int64_t t_start = 0;
  int64_t t_end = 0;
  std::string encoded;  // encode_meta(PageMeta)
};

// Holds the records written by the importer until the collector for the file
// that contains them runs Finish(). One instance per open database.
class PageMetaRegistry {
 public:
  void PutPage(uint32_t ds, int64_t page_no, int64_t t_start, int64_t t_end,
               const std::string& encoded);
  void PutChunk(uint32_t ds, int64_t t_start, int64_t t_end, const std::string& encoded);
  void Clear(uint32_t ds);
  size_t Size() const;

  // Complete payload for a file covering [t0, t1]: every page record that
  // overlaps the range plus the chunk record whose range matches. Returns an
  // empty string when there is nothing to store.
  std::string BuildPayload(uint32_t ds, int64_t t0, int64_t t1) const;

 private:
  struct ChunkEntry {
    int64_t t_start = 0;
    int64_t t_end = 0;
    std::string encoded;
  };

  mutable std::mutex mutex_;
  std::map<uint32_t, std::vector<PageMetaEntry>> pages_;
  std::map<uint32_t, std::vector<ChunkEntry>> chunks_;
};

// Payload layout:
//   u8  version
//   u32 page count, then per page: u32 length + encode_meta bytes
//   u32 chunk length, then the chunk record bytes (empty length when absent)
std::string EncodePayload(const std::vector<const PageMetaEntry*>& pages,
                          const std::string* chunk_encoded);

bool DecodePayload(const std::string& payload, std::vector<PageMeta>* pages, PageMeta* chunk,
                   bool* has_chunk, std::string* error);

class IndbStatsCollector : public rocksdb::TablePropertiesCollector {
 public:
  explicit IndbStatsCollector(std::shared_ptr<PageMetaRegistry> registry)
      : registry_(std::move(registry)) {}

  rocksdb::Status AddUserKey(const rocksdb::Slice& key, const rocksdb::Slice& value,
                             rocksdb::EntryType entry_type, rocksdb::SequenceNumber sequence,
                             uint64_t file_size) override;
  rocksdb::Status Finish(rocksdb::UserCollectedProperties* properties) override;
  rocksdb::UserCollectedProperties GetReadableProperties() const override { return {}; }
  const char* Name() const override { return "Indb.PageStats"; }

 private:
  std::shared_ptr<PageMetaRegistry> registry_;
  bool seen_ = false;
  uint32_t ds_ = 0;
  int64_t min_ts_ = 0;
  int64_t max_ts_ = 0;
};

class IndbStatsCollectorFactory : public rocksdb::TablePropertiesCollectorFactory {
 public:
  explicit IndbStatsCollectorFactory(std::shared_ptr<PageMetaRegistry> registry)
      : registry_(std::move(registry)) {}

  rocksdb::TablePropertiesCollector* CreateTablePropertiesCollector(
      rocksdb::TablePropertiesCollectorFactory::Context context) override {
    (void)context;
    return new IndbStatsCollector(registry_);
  }
  const char* Name() const override { return "Indb.PageStatsFactory"; }

 private:
  std::shared_ptr<PageMetaRegistry> registry_;
};

// Convenience wrapper: extracts and decodes our payload from table properties.
bool DecodeTableProperties(const rocksdb::TableProperties& properties, std::vector<PageMeta>* pages,
                           PageMeta* chunk, bool* has_chunk);

}  // namespace indb
