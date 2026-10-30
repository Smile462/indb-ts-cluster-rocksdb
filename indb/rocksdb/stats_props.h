//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Persists page metadata inside the SST files themselves, using the meta block
// RocksDB natively writes for table properties.
//
// A TablePropertiesCollector observes every key as the table is built. It
// records the time range the file covers, then at Finish() asks the registry
// for the page records that overlap that range and stores them under a single
// user collected property. The payload therefore lives in the properties meta
// block of the very file that holds the corresponding data points.
//
// This is the "natively supported meta block" layout described in the paper:
// no RocksDB source changes are involved.

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

// Name of the user collected property that carries the page records.
extern const char* const kPageStatsProperty;

struct PageMetaEntry {
  int64_t page_no = 0;
  int64_t t_start = 0;
  int64_t t_end = 0;
  std::string encoded;  // encode_meta(PageMeta)
};

// Holds the page records written by the importer until the collector for the
// file that contains them runs Finish(). One instance per open database.
class PageMetaRegistry {
 public:
  void Put(uint32_t ds, int64_t page_no, int64_t t_start, int64_t t_end, const std::string& encoded);
  void Clear(uint32_t ds);
  size_t Size() const;

  // Concatenated payload for every page overlapping [t0, t1] of `ds`.
  // Returns an empty string when nothing overlaps.
  std::string Range(uint32_t ds, int64_t t0, int64_t t1) const;

 private:
  mutable std::mutex mutex_;
  std::map<uint32_t, std::vector<PageMetaEntry>> by_dataset_;
};

// Payload layout: u32 count, then per record u32 length followed by the
// encode_meta bytes.
std::string EncodePayload(const std::vector<const PageMetaEntry*>& entries);
bool DecodePayload(const std::string& payload, std::vector<PageMeta>* pages, std::string* error);

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
bool DecodeTableProperties(const rocksdb::TableProperties& properties, std::vector<PageMeta>* pages);

}  // namespace indb
