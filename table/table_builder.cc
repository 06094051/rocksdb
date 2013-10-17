//  Copyright (c) 2013, Facebook, Inc.  All rights reserved.
//  This source code is licensed under the BSD-style license found in the
//  LICENSE file in the root directory of this source tree. An additional grant
//  of patent rights can be found in the PATENTS file in the same directory.
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "rocksdb/table_builder.h"

#include <assert.h>
#include <map>

#include "rocksdb/comparator.h"
#include "rocksdb/env.h"
#include "rocksdb/filter_policy.h"
#include "rocksdb/options.h"
#include "table/block_builder.h"
#include "table/filter_block.h"
#include "table/format.h"
#include "table/table.h"
#include "util/coding.h"
#include "util/crc32c.h"
#include "util/stop_watch.h"

namespace rocksdb {

namespace {

struct BytewiseLessThan {
  bool operator()(const std::string& key1, const std::string& key2) {
    // smaller entries will be placed in front.
    return comparator->Compare(key1, key2) <= 0;
  }
  const Comparator* comparator = BytewiseComparator();
};

// When writing to a block that requires entries to be sorted by
// `BytewiseComparator`, we can buffer the content to `BytewiseSortedMap`
// before writng to store.
typedef std::map<std::string, std::string, BytewiseLessThan> BytewiseSortedMap;

void AddStats(BytewiseSortedMap& stats, std::string name, uint64_t val) {
  assert(stats.find(name) == stats.end());

  std::string dst;
  PutVarint64(&dst, val);

  stats.insert(
      std::make_pair(name, dst)
  );
}

static bool GoodCompressionRatio(size_t compressed_size, size_t raw_size) {
  // Check to see if compressed less than 12.5%
  return compressed_size < raw_size - (raw_size / 8u);
}

}  // anonymous namespace

struct TableBuilder::Rep {
  Options options;
  Options index_block_options;
  WritableFile* file;
  uint64_t offset = 0;
  Status status;
  BlockBuilder data_block;
  BlockBuilder index_block;
  std::string last_key;

  uint64_t num_entries = 0;
  uint64_t num_data_blocks = 0;
  uint64_t raw_key_size = 0;
  uint64_t raw_value_size = 0;
  uint64_t data_size = 0;

  bool closed = false;  // Either Finish() or Abandon() has been called.
  FilterBlockBuilder* filter_block;

  // We do not emit the index entry for a block until we have seen the
  // first key for the next data block.  This allows us to use shorter
  // keys in the index block.  For example, consider a block boundary
  // between the keys "the quick brown fox" and "the who".  We can use
  // "the r" as the key for the index block entry since it is >= all
  // entries in the first block and < all entries in subsequent
  // blocks.
  //
  // Invariant: r->pending_index_entry is true only if data_block is empty.
  bool pending_index_entry;
  BlockHandle pending_handle;  // Handle to add to index block

  std::string compressed_output;

  Rep(const Options& opt, WritableFile* f)
      : options(opt),
        index_block_options(opt),
        file(f),
        data_block(&options),
        index_block(&index_block_options),
        filter_block(opt.filter_policy == nullptr ? nullptr
                     : new FilterBlockBuilder(opt)),
        pending_index_entry(false) {
    index_block_options.block_restart_interval = 1;
  }
};

TableBuilder::TableBuilder(const Options& options, WritableFile* file,
                           int level)
    : rep_(new Rep(options, file)), level_(level) {
  if (rep_->filter_block != nullptr) {
    rep_->filter_block->StartBlock(0);
  }
}

TableBuilder::~TableBuilder() {
  assert(rep_->closed);  // Catch errors where caller forgot to call Finish()
  delete rep_->filter_block;
  delete rep_;
}

Status TableBuilder::ChangeOptions(const Options& options) {
  // Note: if more fields are added to Options, update
  // this function to catch changes that should not be allowed to
  // change in the middle of building a Table.
  if (options.comparator != rep_->options.comparator) {
    return Status::InvalidArgument("changing comparator while building table");
  }

  // Note that any live BlockBuilders point to rep_->options and therefore
  // will automatically pick up the updated options.
  rep_->options = options;
  rep_->index_block_options = options;
  rep_->index_block_options.block_restart_interval = 1;
  return Status::OK();
}

void TableBuilder::Add(const Slice& key, const Slice& value) {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->num_entries > 0) {
    assert(r->options.comparator->Compare(key, Slice(r->last_key)) > 0);
  }

  const size_t curr_size = r->data_block.CurrentSizeEstimate();
  const size_t estimated_size_after = r->data_block.EstimateSizeAfterKV(key,
      value);
  // Do flush if one of the below two conditions is true:
  // 1) if the current estimated size already exceeds the block size,
  // 2) block_size_deviation is set and the estimated size after appending
  // the kv will exceed the block size and the current size is under the
  // the deviation.
  if (curr_size >= r->options.block_size ||
      (estimated_size_after > r->options.block_size &&
      r->options.block_size_deviation > 0 &&
      (curr_size * 100) >
        r->options.block_size * (100 - r->options.block_size_deviation))) {
    Flush();
  }

  if (r->pending_index_entry) {
    assert(r->data_block.empty());
    r->options.comparator->FindShortestSeparator(&r->last_key, key);
    std::string handle_encoding;
    r->pending_handle.EncodeTo(&handle_encoding);
    r->index_block.Add(r->last_key, Slice(handle_encoding));
    r->pending_index_entry = false;
  }

  if (r->filter_block != nullptr) {
    r->filter_block->AddKey(key);
  }

  r->last_key.assign(key.data(), key.size());
  r->data_block.Add(key, value);
  r->num_entries++;
  r->raw_key_size += key.size();
  r->raw_value_size += value.size();
}

void TableBuilder::Flush() {
  Rep* r = rep_;
  assert(!r->closed);
  if (!ok()) return;
  if (r->data_block.empty()) return;
  assert(!r->pending_index_entry);
  WriteBlock(&r->data_block, &r->pending_handle);
  if (ok()) {
    r->pending_index_entry = true;
    r->status = r->file->Flush();
  }
  if (r->filter_block != nullptr) {
    r->filter_block->StartBlock(r->offset);
  }
  r->data_size = r->offset;
  ++r->num_data_blocks;
}

void TableBuilder::WriteBlock(BlockBuilder* block, BlockHandle* handle) {
  // File format contains a sequence of blocks where each block has:
  //    block_data: uint8[n]
  //    type: uint8
  //    crc: uint32
  assert(ok());
  Rep* r = rep_;
  Slice raw = block->Finish();

  Slice block_contents;
  std::string* compressed = &r->compressed_output;
  CompressionType type;
  // If the use has specified a different compression level for each level,
  // then pick the compresison for that level.
  if (!r->options.compression_per_level.empty()) {
    const int n = r->options.compression_per_level.size();
    // It is possible for level_ to be -1; in that case, we use level
    // 0's compression.  This occurs mostly in backwards compatibility
    // situations when the builder doesn't know what level the file
    // belongs to.  Likewise, if level_ is beyond the end of the
    // specified compression levels, use the last value.
    type = r->options.compression_per_level[std::max(0, std::min(level_, n))];
  } else {
    type = r->options.compression;
  }
  switch (type) {
    case kNoCompression:
      block_contents = raw;
      break;

    case kSnappyCompression: {
      std::string* compressed = &r->compressed_output;
      if (port::Snappy_Compress(r->options.compression_opts, raw.data(),
                                raw.size(), compressed) &&
          GoodCompressionRatio(compressed->size(), raw.size())) {
        block_contents = *compressed;
      } else {
        // Snappy not supported, or not good compression ratio, so just
        // store uncompressed form
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    }
    case kZlibCompression:
      if (port::Zlib_Compress(r->options.compression_opts, raw.data(),
                              raw.size(), compressed) &&
          GoodCompressionRatio(compressed->size(), raw.size())) {
        block_contents = *compressed;
      } else {
        // Zlib not supported, or not good compression ratio, so just
        // store uncompressed form
        block_contents = raw;
        type = kNoCompression;
      }
      break;
    case kBZip2Compression:
      if (port::BZip2_Compress(r->options.compression_opts, raw.data(),
                               raw.size(), compressed) &&
          GoodCompressionRatio(compressed->size(), raw.size())) {
        block_contents = *compressed;
      } else {
        // BZip not supported, or not good compression ratio, so just
        // store uncompressed form
        block_contents = raw;
        type = kNoCompression;
      }
      break;
  }
  WriteRawBlock(block_contents, type, handle);
  r->compressed_output.clear();
  block->Reset();
}

void TableBuilder::WriteRawBlock(const Slice& block_contents,
                                 CompressionType type,
                                 BlockHandle* handle) {
  Rep* r = rep_;
  StopWatch sw(r->options.env, r->options.statistics, WRITE_RAW_BLOCK_MICROS);
  handle->set_offset(r->offset);
  handle->set_size(block_contents.size());
  r->status = r->file->Append(block_contents);
  if (r->status.ok()) {
    char trailer[kBlockTrailerSize];
    trailer[0] = type;
    uint32_t crc = crc32c::Value(block_contents.data(), block_contents.size());
    crc = crc32c::Extend(crc, trailer, 1);  // Extend crc to cover block type
    EncodeFixed32(trailer+1, crc32c::Mask(crc));
    r->status = r->file->Append(Slice(trailer, kBlockTrailerSize));
    if (r->status.ok()) {
      r->offset += block_contents.size() + kBlockTrailerSize;
    }
  }
}

Status TableBuilder::status() const {
  return rep_->status;
}

Status TableBuilder::Finish() {
  Rep* r = rep_;
  Flush();
  assert(!r->closed);
  r->closed = true;

  BlockHandle filter_block_handle, metaindex_block_handle, index_block_handle;

  // Write filter block
  if (ok() && r->filter_block != nullptr) {
    WriteRawBlock(r->filter_block->Finish(), kNoCompression,
                  &filter_block_handle);
  }

  // To make sure stats block is able to keep the accurate size of index
  // block, we will finish writing all index entries here and flush them
  // to storage after metaindex block is written.
  if (ok() && (r->pending_index_entry)) {
      r->options.comparator->FindShortSuccessor(&r->last_key);
      std::string handle_encoding;
      r->pending_handle.EncodeTo(&handle_encoding);
      r->index_block.Add(r->last_key, Slice(handle_encoding));
      r->pending_index_entry = false;
  }

  // Write meta blocks and metaindex block with the following order.
  //    1. [meta block: filter]
  //    2. [meta block: stats]
  //    3. [metaindex block]
  if (ok()) {
    // We use `BytewiseComparator` as the comparator for meta block.
    BlockBuilder meta_index_block(
        r->options.block_restart_interval,
        BytewiseComparator()
    );
    // Key: meta block name
    // Value: block handle to that meta block
    BytewiseSortedMap meta_block_handles;

    // Write filter block.
    if (r->filter_block != nullptr) {
      // Add mapping from "<filter_block_prefix>.Name" to location
      // of filter data.
      std::string key = Table::kFilterBlockPrefix;
      key.append(r->options.filter_policy->Name());
      std::string handle_encoding;
      filter_block_handle.EncodeTo(&handle_encoding);
      meta_block_handles.insert(
          std::make_pair(key, handle_encoding)
      );
    }

    // Write stats block.
    {
      BlockBuilder stats_block(
          r->options.block_restart_interval,
          BytewiseComparator()
      );

      BytewiseSortedMap stats;

      // Add basic stats
      AddStats(stats, TableStatsNames::kRawKeySize, r->raw_key_size);
      AddStats(stats, TableStatsNames::kRawValueSize, r->raw_value_size);
      AddStats(stats, TableStatsNames::kDataSize, r->data_size);
      AddStats(
          stats,
          TableStatsNames::kIndexSize,
          r->index_block.CurrentSizeEstimate() + kBlockTrailerSize
      );
      AddStats(stats, TableStatsNames::kNumEntries, r->num_entries);
      AddStats(stats, TableStatsNames::kNumDataBlocks, r->num_data_blocks);

      for (const auto& stat : stats) {
        stats_block.Add(stat.first, stat.second);
      }

      BlockHandle stats_block_handle;
      WriteBlock(&stats_block, &stats_block_handle);

      std::string handle_encoding;
      stats_block_handle.EncodeTo(&handle_encoding);
      meta_block_handles.insert(
          std::make_pair(Table::kStatsBlock, handle_encoding)
      );
    }  // end of stats block writing

    for (const auto& metablock : meta_block_handles) {
      meta_index_block.Add(metablock.first, metablock.second);
    }

    WriteBlock(&meta_index_block, &metaindex_block_handle);
  }  // meta blocks and metaindex block.

  // Write index block
  if (ok()) {
    WriteBlock(&r->index_block, &index_block_handle);
  }

  // Write footer
  if (ok()) {
    Footer footer;
    footer.set_metaindex_handle(metaindex_block_handle);
    footer.set_index_handle(index_block_handle);
    std::string footer_encoding;
    footer.EncodeTo(&footer_encoding);
    r->status = r->file->Append(footer_encoding);
    if (r->status.ok()) {
      r->offset += footer_encoding.size();
    }
  }
  return r->status;
}

void TableBuilder::Abandon() {
  Rep* r = rep_;
  assert(!r->closed);
  r->closed = true;
}

uint64_t TableBuilder::NumEntries() const {
  return rep_->num_entries;
}

uint64_t TableBuilder::FileSize() const {
  return rep_->offset;
}

}  // namespace rocksdb
