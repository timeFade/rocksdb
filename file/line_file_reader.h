//  Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).

#pragma once
#include <array>

#include "file/sequence_file_reader.h"

namespace ROCKSDB_NAMESPACE {

// 一个在 Env::SequentialFile 之上的包装器，用于从文件中读取文本行。
// 行以 '\n' 为分隔符。最后一行可能包含或不包含尾随的换行符。
// 在内部使用 SequentialFileReader。
class LineFileReader {
 private:
  std::array<char, 8192> buf_;  // 用于缓存读取的数据
  SequentialFileReader sfr_;     // 顺序文件读取器
  IOStatus io_status_;           // 读取操作的IO状态
  const char* buf_begin_ = buf_.data();  // 缓冲区开始位置
  const char* buf_end_ = buf_.data();    // 缓冲区结束位置
  size_t line_number_ = 0;        // 当前读取的行数
  bool at_eof_ = false;           // 是否已到达文件末尾

 public:
  // 参见 SequentialFileReader 构造函数
  template <typename... Args>
  explicit LineFileReader(Args&&... args)
      : sfr_(std::forward<Args&&>(args)...) {}

  // 创建 LineFileReader 实例
  static IOStatus Create(const std::shared_ptr<FileSystem>& fs,
                         const std::string& fname, const FileOptions& file_opts,
                         std::unique_ptr<LineFileReader>* reader,
                         IODebugContext* dbg, RateLimiter* rate_limiter);

  LineFileReader(const LineFileReader&) = delete;  // 禁止拷贝构造函数
  LineFileReader& operator=(const LineFileReader&) = delete;  // 禁止赋值操作符重载

  // 从文件中读取另一行，成功时返回 true 并将行保存到 `out` 中（不包含分隔符），
  // 失败时返回 false。您必须检查 GetStatus() 来确定失败是因为仅到达文件末尾（OK 状态），
  // 还是因为 I/O 错误（其他状态）。
  // 内部的速率限制器将按指定的优先级计费。
  bool ReadLine(std::string* out, Env::IOPriority rate_limiter_priority);

  // 返回最近从 ReadLine 返回的行号。
  // 如果 ReadLine 由于 I/O 错误返回 false，则返回值未指定。
  // 在由于到达文件末尾而导致的 ReadLine 返回 false 后，返回值是最后返回的行号，
  // 或等价地，返回的总行数。
  size_t GetLineNumber() const { return line_number_; }

  // 返回读取过程中遇到的任何错误。错误被视为永久性的，不会尝试使用相同的 LineFileReader 进行重试或恢复。
  const IOStatus& GetStatus() const { return io_status_; }
};

}  // namespace ROCKSDB_NAMESPACE
