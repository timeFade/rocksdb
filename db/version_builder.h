//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
#pragma once

#include <memory>

#include "db/version_edit.h"
#include "rocksdb/file_system.h"
#include "rocksdb/metadata.h"
#include "rocksdb/slice_transform.h"

namespace ROCKSDB_NAMESPACE {

struct ImmutableCFOptions;
class TableCache;
class VersionStorageInfo;
class VersionEdit;
struct FileMetaData;
class InternalStats;
class Version;
class VersionSet;
class ColumnFamilyData;
class CacheReservationManager;
//  构建和管理数据库的版本信息。

// 1. `VersionBuilder` 类：这是一个帮助类，用于在不创建中间版本的情况下，有效地应用一系列编辑操作到特定状态。它包含以下主要方法：
//    - `CheckConsistencyForNumLevels`：检查版本的级别数量的一致性。
//    - `Apply`：应用版本编辑到当前版本状态。
//    - `SaveTo`：将当前状态保存到给定的 `VersionStorageInfo` 中。
//    - `LoadTableHandlers`：加载表处理程序以处理版本的数据文件。
//    - `GetMinOldestBlobFileNumber`：获取最旧的 Blob 文件编号。

// 2. `BaseReferencedVersionBuilder` 类：这是 `VersionBuilder` 的一个包装类，它在构造函数中引用当前版本，并在析构函数中取消引用。这确保了在数据库的互斥锁内正确管理版本的生命周期。

// 3. `NewestFirstBySeqNo` 结构体：这是一个比较器，用于按照序列号降序排列文件元数据。
// A helper class so we can efficiently apply a whole sequence
// of edits to a particular state without creating intermediate
// Versions that contain full copies of the intermediate state.
class VersionBuilder {
 public:
  VersionBuilder(const FileOptions& file_options,
                 const ImmutableCFOptions* ioptions, TableCache* table_cache,
                 VersionStorageInfo* base_vstorage, VersionSet* version_set,
                 std::shared_ptr<CacheReservationManager>
                     file_metadata_cache_res_mgr = nullptr);
  ~VersionBuilder();

  bool CheckConsistencyForNumLevels();
  Status Apply(const VersionEdit* edit);
  Status SaveTo(VersionStorageInfo* vstorage) const;
  Status LoadTableHandlers(
      InternalStats* internal_stats, int max_threads,
      bool prefetch_index_and_filter_in_cache, bool is_initial_load,
      const std::shared_ptr<const SliceTransform>& prefix_extractor,
      size_t max_file_size_for_l0_meta_pin, const ReadOptions& read_options,
      uint8_t block_protection_bytes_per_key);
  uint64_t GetMinOldestBlobFileNumber() const;

 private:
  class Rep;
  std::unique_ptr<Rep> rep_;
};

// A wrapper of version builder which references the current version in
// constructor and unref it in the destructor.
// Both of the constructor and destructor need to be called inside DB Mutex.
class BaseReferencedVersionBuilder {
 public:
  explicit BaseReferencedVersionBuilder(ColumnFamilyData* cfd);
  BaseReferencedVersionBuilder(ColumnFamilyData* cfd, Version* v);
  ~BaseReferencedVersionBuilder();
  VersionBuilder* version_builder() const { return version_builder_.get(); }

 private:
  std::unique_ptr<VersionBuilder> version_builder_;
  Version* version_;
};

class NewestFirstBySeqNo {
 public:
  bool operator()(const FileMetaData* lhs, const FileMetaData* rhs) const {
    assert(lhs);
    assert(rhs);

    if (lhs->fd.largest_seqno != rhs->fd.largest_seqno) {
      return lhs->fd.largest_seqno > rhs->fd.largest_seqno;
    }

    if (lhs->fd.smallest_seqno != rhs->fd.smallest_seqno) {
      return lhs->fd.smallest_seqno > rhs->fd.smallest_seqno;
    }

    // Break ties by file number
    return lhs->fd.GetNumber() > rhs->fd.GetNumber();
  }
};
}  // namespace ROCKSDB_NAMESPACE
