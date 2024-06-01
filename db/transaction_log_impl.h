//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
#pragma once

#include <vector>

#include "db/log_reader.h"
#include "db/version_set.h"
#include "file/filename.h"
#include "logging/logging.h"
#include "options/db_options.h"
#include "port/port.h"
#include "rocksdb/env.h"
#include "rocksdb/options.h"
#include "rocksdb/transaction_log.h"
#include "rocksdb/types.h"

namespace ROCKSDB_NAMESPACE {
// WalFileImpl 类：表示 WAL 文件的实现。它包含了 WAL 文件的一些基本属性，比如文件编号、类型、起始序列号和文件大小等，并提供了一些方法来获取这些属性。

// TransactionLogIteratorImpl 类：事务日志迭代器的实现。它用于迭代读取事务日志中的写入批次（WriteBatch）。
// 该类的构造函数接受一些参数，包括目录路径、不可变数据库选项、读取选项、环境选项、起始序列号、WAL 文件列表、版本集等，
// 并提供了方法来检查迭代器是否有效、获取下一个写入批次、获取当前状态以及获取当前批次的结果等。
class WalFileImpl : public WalFile {
 public:
  WalFileImpl(uint64_t logNum, WalFileType logType, SequenceNumber startSeq,
              uint64_t sizeBytes)
      : logNumber_(logNum),
        type_(logType),
        startSequence_(startSeq),
        sizeFileBytes_(sizeBytes) {}

  std::string PathName() const override {
    if (type_ == kArchivedLogFile) {
      return ArchivedLogFileName("", logNumber_);
    }
    return LogFileName("", logNumber_);
  }

  uint64_t LogNumber() const override { return logNumber_; }

  WalFileType Type() const override { return type_; }

  SequenceNumber StartSequence() const override { return startSequence_; }

  uint64_t SizeFileBytes() const override { return sizeFileBytes_; }

  bool operator<(const WalFile& that) const {
    return LogNumber() < that.LogNumber();
  }

 private:
  uint64_t logNumber_;
  WalFileType type_;
  SequenceNumber startSequence_;
  uint64_t sizeFileBytes_;
};

class TransactionLogIteratorImpl : public TransactionLogIterator {
 public:
  TransactionLogIteratorImpl(
      const std::string& dir, const ImmutableDBOptions* options,
      const TransactionLogIterator::ReadOptions& read_options,
      const EnvOptions& soptions, const SequenceNumber seqNum,
      std::unique_ptr<VectorWalPtr> files, VersionSet const* const versions,
      const bool seq_per_batch, const std::shared_ptr<IOTracer>& io_tracer);

  bool Valid() override;

  void Next() override;

  Status status() override;

  BatchResult GetBatch() override;

 private:
  const std::string& dir_;
  const ImmutableDBOptions* options_;
  const TransactionLogIterator::ReadOptions read_options_;
  const EnvOptions& soptions_;
  SequenceNumber starting_sequence_number_;
  std::unique_ptr<VectorWalPtr> files_;
  // Used only to get latest seq. num
  // TODO(icanadi) can this be just a callback?
  VersionSet const* const versions_;
  const bool seq_per_batch_;
  std::shared_ptr<IOTracer> io_tracer_;

  // State variables
  bool started_;
  bool is_valid_;  // not valid when it starts of.
  Status current_status_;
  size_t current_file_index_;
  std::unique_ptr<WriteBatch> current_batch_;
  std::unique_ptr<log::Reader> current_log_reader_;
  std::string scratch_;
  Status OpenLogFile(const WalFile* log_file,
                     std::unique_ptr<SequentialFileReader>* file);

  struct LogReporter : public log::Reader::Reporter {
    Env* env;
    Logger* info_log;
    void Corruption(size_t bytes, const Status& s) override {
      ROCKS_LOG_ERROR(info_log, "dropping %" ROCKSDB_PRIszt " bytes; %s", bytes,
                      s.ToString().c_str());
    }
    virtual void Info(const char* s) { ROCKS_LOG_INFO(info_log, "%s", s); }
  } reporter_;

  SequenceNumber
      current_batch_seq_;  // sequence number at start of current batch
  SequenceNumber current_last_seq_;  // last sequence in the current batch
  // Reads from transaction log only if the writebatch record has been written
  bool RestrictedRead(Slice* record);
  // Seeks to starting_sequence_number_ reading from start_file_index in files_.
  // If strict is set, then must get a batch starting with
  // starting_sequence_number_.
  void SeekToStartSequence(uint64_t start_file_index = 0, bool strict = false);
  // Implementation of Next. SeekToStartSequence calls it internally with
  // internal=true to let it find next entry even if it has to jump gaps because
  // the iterator may start off from the first available entry but promises to
  // be continuous after that
  void NextImpl(bool internal = false);
  // Check if batch is expected, else return false
  bool IsBatchExpected(const WriteBatch* batch, SequenceNumber expected_seq);
  // Update current batch if a continuous batch is found.
  void UpdateCurrentWriteBatch(const Slice& record);
  Status OpenLogReader(const WalFile* file);
};
}  // namespace ROCKSDB_NAMESPACE
