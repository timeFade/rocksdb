//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "file/readahead_raf.h"

#include <algorithm>
#include <mutex>

#include "file/read_write_util.h"
#include "rocksdb/file_system.h"
#include "util/aligned_buffer.h"
#include "util/rate_limiter_impl.h"

namespace ROCKSDB_NAMESPACE {
namespace {

// ReadaheadRandomAccessFile 类是 FSRandomAccessFile 的子类，增加了预读功能。
class ReadaheadRandomAccessFile : public FSRandomAccessFile {
 public:
  // 构造函数，初始化成员变量并分配预读缓冲区。
  ReadaheadRandomAccessFile(std::unique_ptr<FSRandomAccessFile>&& file,
                            size_t readahead_size)
      : file_(std::move(file)),
        alignment_(file_->GetRequiredBufferAlignment()),
        readahead_size_(Roundup(readahead_size, alignment_)),
        buffer_(),
        buffer_offset_(0) {
    buffer_.Alignment(alignment_);
    buffer_.AllocateNewBuffer(readahead_size_);
  }

  ReadaheadRandomAccessFile(const ReadaheadRandomAccessFile&) = delete;

  ReadaheadRandomAccessFile& operator=(const ReadaheadRandomAccessFile&) =
      delete;

  // 重写 Read 函数，首先尝试从缓冲区读取数据，如果缓存未命中，则从文件读取并填充缓冲区。
  IOStatus Read(uint64_t offset, size_t n, const IOOptions& options,
                Slice* result, char* scratch,
                IODebugContext* dbg) const override {
    // 只有在剩余空间足够时才进行预读
    if (n + alignment_ >= readahead_size_) {
      return file_->Read(offset, n, options, result, scratch, dbg);
    }

    std::unique_lock<std::mutex> lk(lock_);

    size_t cached_len = 0;
    // 检查缓存是否命中，如果命中则从缓存读取数据
    if (TryReadFromCache(offset, n, &cached_len, scratch) &&
        (cached_len == n || buffer_.CurrentSize() < readahead_size_)) {
      // 如果完全命中或已到达文件末尾，返回
      *result = Slice(scratch, cached_len);
      return IOStatus::OK();
    }
    size_t advanced_offset = static_cast<size_t>(offset + cached_len);
    // 对齐偏移量
    size_t chunk_offset = TruncateToPageBoundary(alignment_, advanced_offset);

    IOStatus s = ReadIntoBuffer(chunk_offset, readahead_size_, options, dbg);
    if (s.ok()) {
      // 数据已缓存，读取缓存中的数据
      size_t remaining_len;
      TryReadFromCache(advanced_offset, n - cached_len, &remaining_len,
                       scratch + cached_len);
      *result = Slice(scratch, cached_len + remaining_len);
    }
    return s;
  }

  // 预取数据，将数据填充到缓存中
  IOStatus Prefetch(uint64_t offset, size_t n, const IOOptions& options,
                    IODebugContext* dbg) override {
    if (n < readahead_size_) {
      // 不允许比预读大小小的预取操作
      return IOStatus::OK();
    }

    std::unique_lock<std::mutex> lk(lock_);

    size_t offset_ = static_cast<size_t>(offset);
    size_t prefetch_offset = TruncateToPageBoundary(alignment_, offset_);
    if (prefetch_offset == buffer_offset_) {
      return IOStatus::OK();
    }
    return ReadIntoBuffer(prefetch_offset,
                          Roundup(offset_ + n, alignment_) - prefetch_offset,
                          options, dbg);
  }

  // 获取文件的唯一ID
  size_t GetUniqueId(char* id, size_t max_size) const override {
    return file_->GetUniqueId(id, max_size);
  }

  // 设置文件访问模式的提示
  void Hint(AccessPattern pattern) override { file_->Hint(pattern); }

  // 使缓存失效
  IOStatus InvalidateCache(size_t offset, size_t length) override {
    std::unique_lock<std::mutex> lk(lock_);
    buffer_.Clear();
    return file_->InvalidateCache(offset, length);
  }

  // 是否使用直接IO
  bool use_direct_io() const override { return file_->use_direct_io(); }

 private:
  // 尝试从缓存读取数据，返回是否命中缓存
  bool TryReadFromCache(uint64_t offset, size_t n, size_t* cached_len,
                        char* scratch) const {
    if (offset < buffer_offset_ ||
        offset >= buffer_offset_ + buffer_.CurrentSize()) {
      *cached_len = 0;
      return false;
    }
    uint64_t offset_in_buffer = offset - buffer_offset_;
    *cached_len = std::min(
        buffer_.CurrentSize() - static_cast<size_t>(offset_in_buffer), n);
    memcpy(scratch, buffer_.BufferStart() + offset_in_buffer, *cached_len);
    return true;
  }

  // 将数据读取到缓冲区
  IOStatus ReadIntoBuffer(uint64_t offset, size_t n, const IOOptions& options,
                          IODebugContext* dbg) const {
    if (n > buffer_.Capacity()) {
      n = buffer_.Capacity();
    }
    assert(IsFileSectorAligned(offset, alignment_));
    assert(IsFileSectorAligned(n, alignment_));
    Slice result;
    IOStatus s =
        file_->Read(offset, n, options, &result, buffer_.BufferStart(), dbg);
    if (s.ok()) {
      buffer_offset_ = offset;
      buffer_.Size(result.size());
      assert(result.size() == 0 || buffer_.BufferStart() == result.data());
    }
    return s;
  }

  // 被封装的底层文件
  const std::unique_ptr<FSRandomAccessFile> file_;
  // 文件系统对齐要求
  const size_t alignment_;
  // 预读大小
  const size_t readahead_size_;

  mutable std::mutex lock_;
  // 缓存的预读数据
  mutable AlignedBuffer buffer_;
  // 缓存数据对应的文件偏移量
  mutable uint64_t buffer_offset_;
};

}  // namespace

// 创建带预读功能的随机访问文件
std::unique_ptr<FSRandomAccessFile> NewReadaheadRandomAccessFile(
    std::unique_ptr<FSRandomAccessFile>&& file, size_t readahead_size) {
  std::unique_ptr<FSRandomAccessFile> result(
      new ReadaheadRandomAccessFile(std::move(file), readahead_size));
  return result;
}
}  // namespace ROCKSDB_NAMESPACE
