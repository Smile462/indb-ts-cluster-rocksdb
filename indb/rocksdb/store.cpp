//  Copyright (c) 2026 The in-database time-series clustering authors.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#include "rocksdb/store.h"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <random>
#include <sstream>

namespace indb {
namespace {

constexpr const char* kCfDefault = "default";
constexpr const char* kCfPoints = "points";
constexpr const char* kCfPageMeta = "pagemeta";
constexpr const char* kCfChunkMeta = "chunkmeta";
constexpr const char* kCfPageIndex = "pageindex";
constexpr const char* kCfChunkIndex = "chunkindex";
constexpr const char* kCfCatalog = "catalog";

std::vector<std::string> AllColumnFamilies() {
  return {kCfDefault, kCfPoints, kCfPageMeta, kCfChunkMeta, kCfPageIndex, kCfChunkIndex,
          kCfCatalog};
}

// Keys are big endian so that RocksDB's byte-wise comparator reproduces the
// intended numeric order. Timestamps are sign flipped so negative values sort
// before positive ones.
void PutU32(std::string* out, uint32_t value) {
  for (int i = 3; i >= 0; --i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}
void PutU64(std::string* out, uint64_t value) {
  for (int i = 7; i >= 0; --i) out->push_back(static_cast<char>((value >> (8 * i)) & 0xFF));
}
void PutI64(std::string* out, int64_t value) {
  PutU64(out, static_cast<uint64_t>(value) ^ 0x8000000000000000ULL);
}
uint32_t GetU32(const char* p) {
  uint32_t value = 0;
  for (int i = 0; i < 4; ++i) value = (value << 8) | static_cast<uint8_t>(p[i]);
  return value;
}
uint64_t GetU64(const char* p) {
  uint64_t value = 0;
  for (int i = 0; i < 8; ++i) value = (value << 8) | static_cast<uint8_t>(p[i]);
  return value;
}
int64_t GetI64(const char* p) { return static_cast<int64_t>(GetU64(p) ^ 0x8000000000000000ULL); }

std::string PointKey(uint32_t ds, int64_t ts) {
  std::string key;
  key.reserve(12);
  PutU32(&key, ds);
  PutI64(&key, ts);
  return key;
}
std::string NumberedKey(uint32_t ds, int64_t number) {
  std::string key;
  key.reserve(12);
  PutU32(&key, ds);
  PutU64(&key, static_cast<uint64_t>(number));
  return key;
}
std::string PrefixFor(uint32_t ds) {
  std::string key;
  key.reserve(4);
  PutU32(&key, ds);
  return key;
}

std::string EncodeDataset(const DatasetInfo& info) {
  std::string out;
  PutU32(&out, info.ds);
  PutU32(&out, static_cast<uint32_t>(info.version));
  PutU32(&out, static_cast<uint32_t>(info.config.k));
  PutU32(&out, static_cast<uint32_t>(info.config.l));
  PutU32(&out, static_cast<uint32_t>(info.config.edk_effective()));
  PutU32(&out, static_cast<uint32_t>(info.config.iter_build));
  PutU32(&out, static_cast<uint32_t>(info.config.iter_raw));
  PutU32(&out, static_cast<uint32_t>(info.config.page_points));
  PutU32(&out, static_cast<uint32_t>(info.config.chunk_pages));
  PutU64(&out, static_cast<uint64_t>(info.config.seed));
  PutI64(&out, info.first_ts);
  PutI64(&out, info.last_ts);
  PutI64(&out, info.n_points);
  PutI64(&out, info.n_pages);
  PutI64(&out, info.n_chunks);
  return out;
}

DatasetInfo DecodeDataset(const std::string& bytes) {
  if (bytes.size() < 4 * 8 + 8 + 5 * 8) throw AlgorithmError("catalog record too short");
  const char* p = bytes.data();
  DatasetInfo info;
  info.ds = GetU32(p);
  info.version = static_cast<int32_t>(GetU32(p + 4));
  info.config.k = static_cast<int>(GetU32(p + 8));
  info.config.l = static_cast<int>(GetU32(p + 12));
  info.config.edk = static_cast<int>(GetU32(p + 16));
  info.config.iter_build = static_cast<int>(GetU32(p + 20));
  info.config.iter_raw = static_cast<int>(GetU32(p + 24));
  info.config.page_points = static_cast<int>(GetU32(p + 28));
  info.config.chunk_pages = static_cast<int>(GetU32(p + 32));
  info.config.seed = GetU64(p + 36);
  info.first_ts = GetI64(p + 44);
  info.last_ts = GetI64(p + 52);
  info.n_points = GetI64(p + 60);
  info.n_pages = GetI64(p + 68);
  info.n_chunks = GetI64(p + 76);
  return info;
}

std::string EncodeIndex(int64_t t_start, int64_t t_end) {
  std::string out;
  PutI64(&out, t_start);
  PutI64(&out, t_end);
  return out;
}

std::string EncodeChunkIndex(int64_t page_from, int64_t page_to, int64_t t_start, int64_t t_end) {
  std::string out;
  PutU64(&out, static_cast<uint64_t>(page_from));
  PutU64(&out, static_cast<uint64_t>(page_to));
  PutI64(&out, t_start);
  PutI64(&out, t_end);
  return out;
}

bool ParseLine(const std::string& line, int64_t* ts, double* value, bool* has_ts) {
  if (line.empty()) return false;
  if (line[0] == '#') return false;
  std::stringstream stream(line);
  std::string first;
  if (!std::getline(stream, first, ',')) return false;
  std::string second;
  const bool two_columns = static_cast<bool>(std::getline(stream, second, ','));

  auto trim = [](std::string s) {
    const size_t begin = s.find_first_not_of(" \t\r\n");
    if (begin == std::string::npos) return std::string();
    const size_t end = s.find_last_not_of(" \t\r\n");
    return s.substr(begin, end - begin + 1);
  };
  first = trim(first);
  second = trim(second);
  if (first.empty()) return false;
  if (!(first[0] == '-' || first[0] == '+' || first[0] == '.' || (first[0] >= '0' && first[0] <= '9'))) {
    return false;  // header or comment
  }

  try {
    if (two_columns) {
      *ts = std::stoll(first);
      *value = std::stod(second);
      *has_ts = true;
    } else {
      *value = std::stod(first);
      *has_ts = false;
    }
  } catch (const std::exception&) {
    return false;
  }
  return true;
}

}  // namespace

Store::Store() = default;

Store::~Store() {
  for (rocksdb::ColumnFamilyHandle* handle : handles_) {
    if (db_ && handle) db_->DestroyColumnFamilyHandle(handle);
  }
  db_.reset();
}

std::unique_ptr<Store> Store::Create(const std::string& path) {
  std::unique_ptr<Store> store(new Store());
  rocksdb::Options options;
  options.create_if_missing = true;
  options.create_missing_column_families = true;
  options.compression = rocksdb::kNoCompression;
  options.max_open_files = 4096;

  std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
  for (const std::string& name : AllColumnFamilies()) {
    descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions());
  }
  std::unique_ptr<rocksdb::DB> db;
  const rocksdb::Status status =
      rocksdb::DB::Open(options, path, descriptors, &store->handles_, &db);
  if (!status.ok()) throw AlgorithmError("open/create failed: " + status.ToString());
  store->db_ = std::move(db);
  return store;
}

std::unique_ptr<Store> Store::Open(const std::string& path) {
  std::unique_ptr<Store> store(new Store());
  std::vector<std::string> names;
  {
    rocksdb::Options options;
    options.create_if_missing = false;
    const rocksdb::Status status = rocksdb::DB::ListColumnFamilies(options, path, &names);
    if (!status.ok()) throw AlgorithmError("cannot list column families: " + status.ToString());
  }
  std::vector<rocksdb::ColumnFamilyDescriptor> descriptors;
  for (const std::string& name : names) {
    descriptors.emplace_back(name, rocksdb::ColumnFamilyOptions());
  }
  rocksdb::Options options;
  options.create_if_missing = false;
  std::unique_ptr<rocksdb::DB> db;
  const rocksdb::Status status = rocksdb::DB::Open(options, path, descriptors, &store->handles_, &db);
  if (!status.ok()) throw AlgorithmError("open failed: " + status.ToString());
  store->db_ = std::move(db);
  return store;
}

rocksdb::ColumnFamilyHandle* Store::cf_for(const std::string& name) {
  // handles_ follows the order returned by ListColumnFamilies/descriptors, so
  // resolve by name through the handle itself.
  for (rocksdb::ColumnFamilyHandle* handle : handles_) {
    if (handle->GetName() == name) return handle;
  }
  throw AlgorithmError("missing column family: " + name);
}

void Store::Flush() {
  for (rocksdb::ColumnFamilyHandle* handle : handles_) {
    db_->Flush(rocksdb::FlushOptions(), handle);
  }
}

int64_t Store::ImportPoints(uint32_t ds, const Points& points, const Config& config) {
  if (points.size() == 0) return 0;
  DatasetInfo info;
  info.ds = ds;
  info.config = config;
  info.first_ts = points.ts.front();
  info.last_ts = points.ts.back();
  info.n_points = static_cast<int64_t>(points.size());

  size_t offset = 0;
  WriteBatchOfPoints(ds, points, &offset, config, &info);

  rocksdb::WriteOptions write_options;
  const rocksdb::Status status =
      db_->Put(write_options, cf_for(kCfCatalog), PrefixFor(ds), EncodeDataset(info));
  if (!status.ok()) throw AlgorithmError("catalog write failed: " + status.ToString());
  return info.n_points;
}

void Store::WriteBatchOfPoints(uint32_t ds, const Points& points, size_t* offset,
                               const Config& config, DatasetInfo* info) {
  const size_t total = points.size();
  rocksdb::ColumnFamilyHandle* cf_points = cf_for(kCfPoints);
  rocksdb::ColumnFamilyHandle* cf_page_meta = cf_for(kCfPageMeta);
  rocksdb::ColumnFamilyHandle* cf_page_index = cf_for(kCfPageIndex);
  rocksdb::ColumnFamilyHandle* cf_chunk_meta = cf_for(kCfChunkMeta);
  rocksdb::ColumnFamilyHandle* cf_chunk_index = cf_for(kCfChunkIndex);
  rocksdb::WriteOptions write_options;

  std::vector<PageMeta> chunk_pages;
  int64_t page_no = info->n_pages;
  int64_t chunk_no = info->n_chunks;

  while (*offset < total) {
    const size_t begin = *offset;
    const size_t end = std::min(total, begin + static_cast<size_t>(config.page_points));
    Points page;
    page.ts.assign(points.ts.begin() + begin, points.ts.begin() + end);
    page.values.assign(points.values.begin() + begin, points.values.begin() + end);

    rocksdb::WriteBatch batch;
    for (size_t i = 0; i < page.size(); ++i) {
      batch.Put(cf_points, PointKey(ds, page.ts[i]),
                rocksdb::Slice(reinterpret_cast<const char*>(&page.values[i]), sizeof(double)));
    }

    // Deterministic per page seed so rebuilding a page reproduces the same
    // clustering.
    const uint64_t page_seed =
        config.seed ^ (static_cast<uint64_t>(page_no + 1) * 0x9E3779B97F4A7C15ULL);
    const PageMeta meta = build_page_meta(page, config, page_seed);
    const std::string encoded = encode_meta(meta);
    batch.Put(cf_page_meta, NumberedKey(ds, page_no), encoded);
    batch.Put(cf_page_index, NumberedKey(ds, page_no), EncodeIndex(meta.t_start, meta.t_end));

    const rocksdb::Status status = db_->Write(write_options, &batch);
    if (!status.ok()) throw AlgorithmError("page write failed: " + status.ToString());

    chunk_pages.push_back(meta);
    ++page_no;
    *offset = end;

    const bool chunk_full = static_cast<int>(chunk_pages.size()) >= config.chunk_pages;
    const bool series_done = *offset >= total;
    if (chunk_full || series_done) {
      const PageMeta chunk = build_chunk_meta(chunk_pages, config);
      const int64_t page_from = page_no - static_cast<int64_t>(chunk_pages.size());
      const int64_t page_to = page_no - 1;
      rocksdb::WriteBatch chunk_batch;
      chunk_batch.Put(cf_chunk_meta, NumberedKey(ds, chunk_no), encode_meta(chunk));
      chunk_batch.Put(cf_chunk_index, NumberedKey(ds, chunk_no),
                      EncodeChunkIndex(page_from, page_to, chunk.t_start, chunk.t_end));
      const rocksdb::Status chunk_status = db_->Write(write_options, &chunk_batch);
      if (!chunk_status.ok()) throw AlgorithmError("chunk write failed: " + chunk_status.ToString());
      ++chunk_no;
      chunk_pages.clear();
    }
  }

  info->n_pages = page_no;
  info->n_chunks = chunk_no;
}

int64_t Store::ImportCsv(uint32_t ds, const std::string& csv_path, const Config& config,
                         bool skip_header) {
  std::ifstream input(csv_path);
  if (!input) throw AlgorithmError("cannot open CSV: " + csv_path);

  Points points;
  std::string line;
  int64_t row = 0;
  bool skipped = !skip_header;
  while (std::getline(input, line)) {
    int64_t ts = 0;
    double value = 0.0;
    bool has_ts = false;
    if (!ParseLine(line, &ts, &value, &has_ts)) continue;
    if (!skipped) {
      skipped = true;
      continue;
    }
    points.ts.push_back(has_ts ? ts : row);
    points.values.push_back(value);
    ++row;
  }
  if (points.size() == 0) throw AlgorithmError("CSV contains no points: " + csv_path);
  return ImportPoints(ds, points, config);
}

DatasetInfo Store::Describe(uint32_t ds) const {
  std::string value;
  const rocksdb::Status status =
      db_->Get(rocksdb::ReadOptions(), const_cast<Store*>(this)->cf_for(kCfCatalog),
               PrefixFor(ds), &value);
  if (!status.ok()) throw AlgorithmError("dataset " + std::to_string(ds) + " not found");
  return DecodeDataset(value);
}

std::vector<DatasetInfo> Store::ListDatasets() const {
  std::vector<DatasetInfo> out;
  std::unique_ptr<rocksdb::Iterator> it(
      db_->NewIterator(rocksdb::ReadOptions(), const_cast<Store*>(this)->cf_for(kCfCatalog)));
  for (it->SeekToFirst(); it->Valid(); it->Next()) {
    out.push_back(DecodeDataset(it->value().ToString()));
  }
  return out;
}

Points Store::ReadRange(uint32_t ds, int64_t t0, int64_t t1) const {
  Points points;
  rocksdb::ColumnFamilyHandle* cf = const_cast<Store*>(this)->cf_for(kCfPoints);
  std::unique_ptr<rocksdb::Iterator> it(db_->NewIterator(rocksdb::ReadOptions(), cf));
  for (it->Seek(PointKey(ds, t0)); it->Valid(); it->Next()) {
    const rocksdb::Slice key = it->key();
    if (key.size() != 12) break;
    if (GetU32(key.data()) != ds) break;
    const int64_t ts = GetI64(key.data() + 4);
    if (ts > t1) break;
    double value = 0.0;
    std::memcpy(&value, it->value().data(), sizeof(double));
    points.ts.push_back(ts);
    points.values.push_back(value);
  }
  return points;
}

Points Store::ReadPage(uint32_t ds, int64_t page_no) const {
  std::string index;
  const rocksdb::Status status = db_->Get(rocksdb::ReadOptions(),
                                          const_cast<Store*>(this)->cf_for(kCfPageIndex),
                                          NumberedKey(ds, page_no), &index);
  if (!status.ok() || index.size() < 16) return Points();
  const int64_t t_start = GetI64(index.data());
  const int64_t t_end = GetI64(index.data() + 8);
  return ReadRange(ds, t_start, t_end);
}

std::vector<int64_t> Store::PagesCovering(uint32_t ds, int64_t t0, int64_t t1) const {
  const DatasetInfo info = Describe(ds);
  std::vector<int64_t> pages;
  if (info.n_pages <= 0) return pages;

  auto range_at = [&](int64_t page_no, int64_t* start, int64_t* end) {
    std::string index;
    const rocksdb::Status status = db_->Get(rocksdb::ReadOptions(),
                                            const_cast<Store*>(this)->cf_for(kCfPageIndex),
                                            NumberedKey(ds, page_no), &index);
    if (!status.ok() || index.size() < 16) return false;
    *start = GetI64(index.data());
    *end = GetI64(index.data() + 8);
    return true;
  };

  // Page time ranges are increasing, so the first and last overlapping page can
  // be located with two binary searches that read O(log n) index records.
  int64_t low = 0;
  int64_t high = info.n_pages - 1;
  int64_t first = info.n_pages;
  while (low <= high) {
    const int64_t mid = low + (high - low) / 2;
    int64_t start = 0;
    int64_t end = 0;
    if (!range_at(mid, &start, &end)) break;
    if (end >= t0) {
      first = mid;
      high = mid - 1;
    } else {
      low = mid + 1;
    }
  }
  if (first >= info.n_pages) return pages;

  low = first;
  high = info.n_pages - 1;
  int64_t last = first - 1;
  while (low <= high) {
    const int64_t mid = low + (high - low) / 2;
    int64_t start = 0;
    int64_t end = 0;
    if (!range_at(mid, &start, &end)) break;
    if (start <= t1) {
      last = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  for (int64_t p = first; p <= last && p < info.n_pages; ++p) pages.push_back(p);
  return pages;
}

std::vector<int64_t> Store::ChunksCovering(uint32_t ds, int64_t t0, int64_t t1) const {
  const DatasetInfo info = Describe(ds);
  std::vector<int64_t> chunks;
  if (info.n_chunks <= 0) return chunks;

  auto range_at = [&](int64_t chunk_no, int64_t* start, int64_t* end) {
    std::string index;
    const rocksdb::Status status = db_->Get(rocksdb::ReadOptions(),
                                            const_cast<Store*>(this)->cf_for(kCfChunkIndex),
                                            NumberedKey(ds, chunk_no), &index);
    if (!status.ok() || index.size() < 32) return false;
    *start = GetI64(index.data() + 16);
    *end = GetI64(index.data() + 24);
    return true;
  };

  int64_t low = 0;
  int64_t high = info.n_chunks - 1;
  int64_t first = info.n_chunks;
  while (low <= high) {
    const int64_t mid = low + (high - low) / 2;
    int64_t start = 0;
    int64_t end = 0;
    if (!range_at(mid, &start, &end)) break;
    if (end >= t0) {
      first = mid;
      high = mid - 1;
    } else {
      low = mid + 1;
    }
  }
  if (first >= info.n_chunks) return chunks;

  low = first;
  high = info.n_chunks - 1;
  int64_t last = first - 1;
  while (low <= high) {
    const int64_t mid = low + (high - low) / 2;
    int64_t start = 0;
    int64_t end = 0;
    if (!range_at(mid, &start, &end)) break;
    if (start <= t1) {
      last = mid;
      low = mid + 1;
    } else {
      high = mid - 1;
    }
  }
  for (int64_t c = first; c <= last && c < info.n_chunks; ++c) chunks.push_back(c);
  return chunks;
}

PageMeta Store::ReadPageMeta(uint32_t ds, int64_t page_no, int64_t* bytes) const {
  std::string value;
  const rocksdb::Status status = db_->Get(rocksdb::ReadOptions(),
                                          const_cast<Store*>(this)->cf_for(kCfPageMeta),
                                          NumberedKey(ds, page_no), &value);
  if (!status.ok()) throw AlgorithmError("page metadata " + std::to_string(page_no) + " missing");
  if (bytes != nullptr) *bytes += static_cast<int64_t>(value.size());
  return decode_meta(value);
}

Store::ChunkRecord Store::ReadChunkMeta(uint32_t ds, int64_t chunk_no, int64_t* bytes) const {
  std::string value;
  const rocksdb::Status status = db_->Get(rocksdb::ReadOptions(),
                                          const_cast<Store*>(this)->cf_for(kCfChunkMeta),
                                          NumberedKey(ds, chunk_no), &value);
  if (!status.ok()) throw AlgorithmError("chunk metadata " + std::to_string(chunk_no) + " missing");
  if (bytes != nullptr) *bytes += static_cast<int64_t>(value.size());
  ChunkRecord record;
  record.meta = decode_meta(value);

  std::string index;
  const rocksdb::Status index_status = db_->Get(rocksdb::ReadOptions(),
                                                const_cast<Store*>(this)->cf_for(kCfChunkIndex),
                                                NumberedKey(ds, chunk_no), &index);
  if (index_status.ok() && index.size() >= 32) {
    record.page_from = static_cast<int64_t>(GetU64(index.data()));
    record.page_to = static_cast<int64_t>(GetU64(index.data() + 8));
  }
  return record;
}

Config Store::ResolveConfig(const QueryOptions& options, const DatasetInfo& info) const {
  Config config = info.config;
  if (options.k > 0) config.k = options.k;
  if (options.l > 0) config.l = options.l;
  if (options.page_points > 0) config.page_points = options.page_points;
  if (options.chunk_pages > 0) config.chunk_pages = options.chunk_pages;
  config.edk = options.edk > 0 ? options.edk : config.edk;
  if (config.k != info.config.k || config.l != info.config.l ||
      config.edk_effective() != info.config.edk_effective()) {
    throw AlgorithmError("query parameters do not match the stored dataset");
  }
  return config;
}

QueryResult Store::KShapeRaw(const QueryOptions& options) {
  const DatasetInfo info = Describe(options.ds);
  const Config config = ResolveConfig(options, info);
  const Points points = ReadRange(options.ds, options.t0, options.t1);
  QueryResult result = kshape_raw(points, config);
  result.stats.raw_points_read = static_cast<int64_t>(points.size());
  return result;
}

QueryResult Store::MShapeRaw(const QueryOptions& options) {
  const DatasetInfo info = Describe(options.ds);
  const Config config = ResolveConfig(options, info);
  const Points points = ReadRange(options.ds, options.t0, options.t1);
  QueryResult result = mshape_raw(points, config);
  result.stats.raw_points_read = static_cast<int64_t>(points.size());
  return result;
}

QueryResult Store::KShape(const QueryOptions& options) {
  const DatasetInfo info = Describe(options.ds);
  const Config config = ResolveConfig(options, info);

  std::vector<PageRef> pages;
  int64_t meta_bytes = 0;
  for (int64_t page_no : PagesCovering(options.ds, options.t0, options.t1)) {
    PageRef ref;
    ref.page_no = page_no;
    ref.meta = ReadPageMeta(options.ds, page_no, &meta_bytes);
    pages.push_back(ref);
  }

  std::vector<ChunkRef> chunks;
  if (options.level == Level::Chunk) {
    for (int64_t chunk_no : ChunksCovering(options.ds, options.t0, options.t1)) {
      const ChunkRecord record = ReadChunkMeta(options.ds, chunk_no, &meta_bytes);
      ChunkRef ref;
      ref.chunk_no = chunk_no;
      ref.page_from = record.page_from;
      ref.page_to = record.page_to;
      ref.meta = record.meta;
      chunks.push_back(ref);
    }
  }

  const RawPageLoader loader = [this, options](int64_t page_no, Points* out) {
    *out = ReadPage(options.ds, page_no);
    return out->size() > 0;
  };

  QueryResult result =
      kshape_query(chunks, pages, options.t0, options.t1, config, options.level, loader);
  result.stats.meta_bytes_read = meta_bytes;
  return result;
}

QueryResult Store::MShape(const QueryOptions& options) {
  const DatasetInfo info = Describe(options.ds);
  const Config config = ResolveConfig(options, info);

  std::vector<PageRef> pages;
  std::vector<int64_t> page_numbers = PagesCovering(options.ds, options.t0, options.t1);
  int64_t meta_bytes = 0;
  for (int64_t page_no : page_numbers) {
    PageRef ref;
    ref.page_no = page_no;
    ref.meta = ReadPageMeta(options.ds, page_no, &meta_bytes);
    pages.push_back(ref);
  }

  std::vector<ChunkRef> chunks;
  if (options.level == Level::Chunk) {
    for (int64_t chunk_no : ChunksCovering(options.ds, options.t0, options.t1)) {
      const ChunkRecord record = ReadChunkMeta(options.ds, chunk_no, &meta_bytes);
      ChunkRef ref;
      ref.chunk_no = chunk_no;
      ref.page_from = record.page_from;
      ref.page_to = record.page_to;
      ref.meta = record.meta;
      chunks.push_back(ref);
    }
  }

  const RawPageLoader loader = [this, options](int64_t page_no, Points* out) {
    *out = ReadPage(options.ds, page_no);
    return out->size() > 0;
  };

  // Medoid-Shape samples chunks by size, then pages, then subsequences, the
  // same order the reference implementation uses.
  std::mt19937 sampler(static_cast<uint32_t>(config.seed));
  const SampledSource source = [this, options, config, page_numbers, &sampler](
                                   int max_sequences, std::vector<Vec>* out) {
    std::vector<int64_t> candidate_chunks = page_numbers;
    std::vector<double> weights;
    weights.reserve(candidate_chunks.size());
    double total_weight = 0.0;
    for (int64_t page_no : candidate_chunks) {
      const Points page = ReadPage(options.ds, page_no);
      const double weight = static_cast<double>(std::max<size_t>(1, page.size()));
      weights.push_back(weight);
      total_weight += weight;
    }
    if (total_weight <= 0.0) return false;

    std::uniform_real_distribution<double> uniform(0.0, total_weight);
    std::vector<Vec> collected;
    for (int attempt = 0; attempt < 4 * max_sequences && static_cast<int>(collected.size()) < max_sequences;
         ++attempt) {
      // Page level sampling: keep a page with probability r.
      std::uniform_real_distribution<double> coin(0.0, 1.0);
      const int index = static_cast<int>(uniform(sampler) / total_weight * weights.size());
      if (index >= static_cast<int>(candidate_chunks.size())) continue;
      if (coin(sampler) > config.r) continue;
      const Points page = ReadPage(options.ds, candidate_chunks[static_cast<size_t>(index)]);
      const std::vector<Vec> seqs = grid_subsequences(page, config.l);
      for (const Vec& seq : seqs) {
        if (static_cast<int>(collected.size()) >= max_sequences) break;
        collected.push_back(seq);
      }
    }
    if (collected.empty()) return false;
    *out = std::move(collected);
    return true;
  };

  QueryResult result =
      mshape_query(chunks, pages, options.t0, options.t1, config, options.level, loader, source);
  result.stats.meta_bytes_read = meta_bytes;
  return result;
}

}  // namespace indb
