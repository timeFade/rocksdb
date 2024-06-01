// 版权声明
// 包含两个许可证的声明：GPLv2（在根目录的 COPYING 文件中找到）和 Apache 2.0 许可证（在根目录的 LICENSE.Apache 文件中找到）。
//
// 该文件实现了文件预取缓冲区的功能，用于从文件中加载和读取数据。

#pragma once

#include <algorithm>
#include <atomic>
#include <deque>
#include <sstream>
#include <string>

#include "file/readahead_file_info.h"  // 文件预读信息
#include "monitoring/statistics_impl.h"  // 监控统计
#include "port/port.h"  // 平台相关的宏定义
#include "rocksdb/env.h"  // RocksDB 环境
#include "rocksdb/file_system.h"  // RocksDB 文件系统
#include "rocksdb/options.h"  // RocksDB 选项
#include "util/aligned_buffer.h"  // 对齐缓冲区
#include "util/autovector.h"  // 自动向量
#include "util/stop_watch.h"  // 计时器
//这个类是用来实现文件预取功能的。在磁盘读取数据时，通常会预取一些未来可能需要的数据，以提高读取效率。
// 这个类负责管理预取缓冲区，根据需要从文件中预取数据，并在需要时将预取的数据提供给用户。它可以异步地进行预取操作，
// 并在预取数据被使用后自动填充缓冲区。此外，它还可以根据读取模式动态地调整预取大小，以优化性能。

namespace ROCKSDB_NAMESPACE {

// 默认的缩减值
#define DEFAULT_DECREMENT 8 * 1024

// 读取预取参数
struct ReadaheadParams {
  ReadaheadParams() {}

  // 初始预取大小
  size_t initial_readahead_size = 0;

  // 最大预取大小
  // 如果 max_readahead_size > readahead_size，则每次 IO 都会将预取大小加倍，直到达到 max_readahead_size 为止。
  // 通常将其设置为 initial_readahead_size 的倍数。initial_readahead_size 应大于等于 initial_readahead_size。
  size_t max_readahead_size = 0;

  // 如果为 true，则在对 num_file_reads_for_auto_readahead 进行顺序扫描后，RocksDB 会隐式地启用预取。
  bool implicit_auto_readahead = false;

  // TODO akanksha - 当 BlockPrefetcher 重构时移除 num_file_reads。
  uint64_t num_file_reads = 0;
  uint64_t num_file_reads_for_auto_readahead = 0;

  // 维护包含预取数据的缓冲区数量。如果 num_buffers > 1，则在这些缓冲区为空时会异步填充它们。
  size_t num_buffers = 1;
};

// 缓冲区信息结构体
struct BufferInfo {
  void ClearBuffer() {
    buffer_.Clear();
    initial_end_offset_ = 0;
    async_req_len_ = 0;
  }

  AlignedBuffer buffer_;  // 对齐缓冲区

  uint64_t offset_ = 0;  // 偏移量

  // 在异步读取流程中使用的参数。
  // ReadAsync 中请求的长度。
  size_t async_req_len_ = 0;

  // async_read_in_progress 可以用作互斥体。回调可以更新缓冲区及其大小，但 async_read_in_progress 仅由主线程设置。
  bool async_read_in_progress_ = false;

  // io_handle 由底层文件系统分配并在异步读取时使用。
  void* io_handle_ = nullptr;

  IOHandleDeleter del_fn_ = nullptr;  // io_handle 的删除函数

  // 初始结束偏移量用于跟踪原始调用的缓冲区的末尾偏移量。
  // 这在对 BlockBasedTableIterator 进行预读大小优化时很有用。
  uint64_t initial_end_offset_ = 0;

  // 检查缓冲区是否包含指定偏移量和长度的数据。
  bool IsDataBlockInBuffer(uint64_t offset, size_t length) {
    assert(async_read_in_progress_ == false);
    return (offset >= offset_ &&
            offset + length <= offset_ + buffer_.CurrentSize());
  }

  // 检查指定的偏移量是否在缓冲区内。
  bool IsOffsetInBuffer(uint64_t offset) {
    assert(async_read_in_progress_ == false);
    return (offset >= offset_ && offset < offset_ + buffer_.CurrentSize());
  }

  // 检查缓冲区是否包含数据。
  bool DoesBufferContainData() {
    assert(async_read_in_progress_ == false);
    return buffer_.CurrentSize() > 0;
  }

  // 检查缓冲区是否已过时。
  bool IsBufferOutdated(uint64_t offset) {
    return (!async_read_in_progress_ && DoesBufferContainData() &&
            offset >= offset_ + buffer_.CurrentSize());
  }

  // 检查缓冲区是否带有异步读取进度并已过时。
  bool IsBufferOutdatedWithAsyncProgress(uint64_t offset) {
    return (async_read_in_progress_ && io_handle_ != nullptr &&
            offset >= offset_ + async_req_len_);
  }

  // 检查指定的偏移量是否在带有异步读取进度的缓冲区内。
  bool IsOffsetInBufferWithAsyncProgress(uint64_t offset) {
    return (async_read_in_progress_ && offset >= offset_ &&
            offset < offset_ + async_req_len_);
  }

  // 获取当前缓冲区的大小。
  size_t CurrentSize() { return buffer_.CurrentSize(); }
};

// 文件预取缓冲区的使用情况枚举
enum class FilePrefetchBufferUsage {
  kTableOpenPrefetchTail,  // 表打开时的预取
  kUserScanPrefetch,       // 用户扫描时的预取
  kUnknown,                // 未知
};

// 文件预取缓冲区类
class FilePrefetchBuffer {
 public:
  // 构造函数。
  //
  // 所有参数都是可选的。
  // readahead_params: 控制预读行为的参数。
  // enable: 控制是否启用从缓冲区读取。如果为 false，则 TryReadFromCache() 始终返回 false，我们仅在 track_min_offset = true 时记录最小偏移量。
  //        请参阅下面关于 mmap 读取的注释。
  // track_min_offset: 跟踪传递给 TryReadFromCache() 的最小偏移量，并对其进行统计。用于文件尾/元数据的自适应预读
  // opts : 要使用的 IO 选项。
  // reader : 文件读取器。
  // offset : 要开始读取的文件偏移量。
  // n : 要读取的字节数。
  //
  // 返回 Status。
  Status Prefetch(const IOOptions& opts, RandomAccessFileReader* reader,
                  uint64_t offset, size_t n);

  // 异步请求从文件中读取数据。
  // 如果数据已存在于缓冲区中，则结果将被更新。
  // reader : 文件读取器。
  // offset : 要开始读取的文件偏移量。
  // n : 要读取的字节数。
  // result : 如果数据已经存在于缓冲区中，则结果将被更新为该数据。
  //
  // 如果数据已经存在于缓冲区中，则返回 Status::OK，否则会发送异步请求并返回 Status::TryAgain。
  Status PrefetchAsync(const IOOptions& opts, RandomAccessFileReader* reader,
                       uint64_t offset, size_t n, Slice* result);

  // 如果文件读取的数据在缓冲区中，则尝试从该缓冲区读取。
  // 它处理跟踪 track_min_offset = true 时的最小读取偏移量。
  // 当设置了 readahead_size 作为构造函数的一部分时，它还执行指数预读。
  //
  // opts : 要使用的 IO 选项。
  // reader : 文件读取器。
  // offset : 文件偏移量。
  // n : 字节数。
  // result : 输出缓冲区，用于存放数据。
  // s : 输出状态。
  // for_compaction : 如果缓存读取是为了压缩读取，则为 true。
  bool TryReadFromCache(const IOOptions& opts, RandomAccessFileReader* reader,
                        uint64_t offset, size_t n, Slice* result, Status* s,
                        bool for_compaction = false);

  // TryReadFromCache() 的重载版本，不跟踪最小读取偏移量。
  bool TryReadFromCacheUntracked(const IOOptions& opts,
                                 RandomAccessFileReader* reader,
                                 uint64_t offset, size_t n, Slice* result,
                                 Status* s,
                                 bool for_compaction = false);

  // 返回 TryReadFromCache() 传递的最小偏移量。仅当 track_min_offset = true 时才会跟踪。
  size_t min_offset_read() const { return min_offset_read_; }

  // 获取预取偏移量。
  size_t GetPrefetchOffset() const { return bufs_.front()->offset_; }

  // 在隐式自动预取情况下调用。
  void UpdateReadPattern(const uint64_t& offset, const size_t& len,
                         bool decrease_readaheadsize) {
    if (decrease_readaheadsize) {
      DecreaseReadAheadIfEligible(offset, len);
    }
    prev_offset_ = offset;
    prev_len_ = len;
    explicit_prefetch_submitted_ = false;
  }

  // 获取预读状态信息。
  void GetReadaheadState(ReadaheadFileInfo::ReadaheadInfo* readahead_info) {
    readahead_info->readahead_size = readahead_size_;
    readahead_info->num_file_reads = num_file_reads_;
  }

  // 如果符合条件，降低预读大小。
  void DecreaseReadAheadIfEligible(uint64_t offset, size_t size,
                                   size_t value = DEFAULT_DECREMENT);

  // 异步读取的回调函数。
  void PrefetchAsyncCallback(FSReadRequest& req, void* cb_arg);

  // 用于测试的方法，用于获取缓冲区的偏移量和大小信息。
  void TEST_GetBufferOffsetandSize(
      std::vector<std::pair<uint64_t, size_t>>& buffer_info) {
    for (size_t i = 0; i < bufs_.size(); i++) {
      buffer_info[i].first = bufs_[i]->offset_;
      buffer_info[i].second = bufs_[i]->async_read_in_progress_
                                  ? bufs_[i]->async_req_len_
                                  : bufs_[i]->CurrentSize();
    }
  }

 private:
  // 根据对齐和缓冲区中的数据计算舍入的偏移量和长度。如果需要，会分配新的缓冲区或调整尾部。
  void PrepareBufferForRead(BufferInfo* buf, size_t alignment, uint64_t offset,
                            size_t roundup_len, bool ref
                            bool refit_tail,
                            uint64_t& aligned_useful_len);

  // 中止过时的 IO 请求。
  void AbortOutdatedIO(uint64_t offset);

  // 中止所有 IO 请求。
  void AbortAllIOs();

  // 清除过时数据。
  void ClearOutdatedData(uint64_t offset, size_t len);

  // 如果需要，调用 Poll API 来检查任何待处理的异步请求。
  void PollIfNeeded(uint64_t offset, size_t len);

  // 内部预取方法。
  Status PrefetchInternal(const IOOptions& opts, RandomAccessFileReader* reader,
                          uint64_t offset, size_t length, size_t readahead_size,
                          bool& copy_to_third_buffer);

  // 读取数据。
  Status Read(BufferInfo* buf, const IOOptions& opts,
              RandomAccessFileReader* reader, uint64_t read_len,
              uint64_t aligned_useful_len, uint64_t start_offset);

  // 异步读取数据。
  Status ReadAsync(BufferInfo* buf, const IOOptions& opts,
                   RandomAccessFileReader* reader, uint64_t read_len,
                   uint64_t start_offset);

  // 将数据从 src 复制到 overlap_buf_。
  void CopyDataToBuffer(BufferInfo* src, uint64_t& offset, size_t& length);

  // 检查块是否顺序读取。
  bool IsBlockSequential(const size_t& offset);

  // 重置隐式自动预取的值。
  void ResetValues();

  // 判断是否符合预取条件。
  bool IsEligibleForPrefetch(uint64_t offset, size_t n);

  // 判断是否可以进一步预取。
  bool IsEligibleForFurtherPrefetching();

  // 释放空缓冲区。
  void FreeEmptyBuffers();

  // 处理重叠的数据。
  Status HandleOverlappingData(const IOOptions& opts,
                               RandomAccessFileReader* reader, uint64_t offset,
                               size_t length, size_t readahead_size,
                               bool& copy_to_third_buffer, uint64_t& tmp_offset,
                               size_t& tmp_length);

  // 读取块数据以调整预读大小。
  void ReadAheadSizeTuning(BufferInfo* buf, bool read_curr_block,
                           bool refit_tail, uint64_t prev_buf_end_offset,
                           size_t alignment, size_t length,
                           size_t readahead_size, uint64_t& offset,
                           uint64_t& end_offset, size_t& read_len,
                           uint64_t& aligned_useful_len);

  // 分配和释放缓冲区的相关 API。

  // 缓冲区队列是否为空。
  bool IsBufferQueueEmpty();

  // 获取第一个缓冲区。
  BufferInfo* GetFirstBuffer();

  // 获取最后一个缓冲区。
  BufferInfo* GetLastBuffer();

  // 已分配的缓冲区数量。
  size_t NumBuffersAllocated();

  // 分配缓冲区。
  void AllocateBuffer();

  // 如果缓冲区为空，则分配缓冲区。
  void AllocateBufferIfEmpty();

  // 释放第一个缓冲区。
  void FreeFrontBuffer();

  // 释放最后一个缓冲区。
  void FreeLastBuffer();

  // 释放所有缓冲区。
  void FreeAllBuffers();
};
}
