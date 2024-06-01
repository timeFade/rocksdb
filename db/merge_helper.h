//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
#pragma once

#include <deque>
#include <string>
#include <vector>

#include "db/merge_context.h"
#include "db/range_del_aggregator.h"
#include "db/snapshot_checker.h"
#include "db/wide/wide_column_serialization.h"
#include "rocksdb/compaction_filter.h"
#include "rocksdb/env.h"
#include "rocksdb/merge_operator.h"
#include "rocksdb/slice.h"
#include "rocksdb/wide_columns.h"
#include "util/stop_watch.h"

namespace ROCKSDB_NAMESPACE {

class Comparator;
class Iterator;
class Logger;
class MergeOperator;
class Statistics;
class SystemClock;
class BlobFetcher;
class PrefetchBufferCollection;
struct CompactionIterationStats;

// MergeHelper 类用于处理 RocksDB 的合并操作
class MergeHelper {
 public:
  MergeHelper(Env* env, const Comparator* user_comparator,
              const MergeOperator* user_merge_operator,
              const CompactionFilter* compaction_filter, Logger* logger,
              bool assert_valid_internal_key, SequenceNumber latest_snapshot,
              const SnapshotChecker* snapshot_checker = nullptr, int level = 0,
              Statistics* stats = nullptr,
              const std::atomic<bool>* shutting_down = nullptr);

  // MergeOperator::FullMergeV3() 的包装器，用于记录性能统计数据。
  // 如果是用户读取操作，设置 `update_num_ops_stats` 为 true，以便更新相应的统计数据。
  // 返回以下状态之一：
  // - OK: 条目合并成功。
  // - Corruption: 合并操作符报告合并失败。当 `op_failure_scope` 不为 nullptr 时，损坏的范围将存储在 `*op_failure_scope` 中。

  // 空标签类型，用于区分重载
  struct NoBaseValueTag {};
  static constexpr NoBaseValueTag kNoBaseValue{};

  struct PlainBaseValueTag {};
  static constexpr PlainBaseValueTag kPlainBaseValue{};

  struct WideBaseValueTag {};
  static constexpr WideBaseValueTag kWideBaseValue{};

  // 无基础值的合并
  template <typename... ResultTs>
  static Status TimedFullMerge(const MergeOperator* merge_operator,
                               const Slice& key, NoBaseValueTag,
                               const std::vector<Slice>& operands,
                               Logger* logger, Statistics* statistics,
                               SystemClock* clock, bool update_num_ops_stats,
                               MergeOperator::OpFailureScope* op_failure_scope,
                               ResultTs... results) {
    MergeOperator::MergeOperationInputV3::ExistingValue existing_value;

    return TimedFullMergeImpl(
        merge_operator, key, std::move(existing_value), operands, logger,
        statistics, clock, update_num_ops_stats, op_failure_scope, results...);
  }

  // 使用普通基础值的合并
  template <typename... ResultTs>
  static Status TimedFullMerge(
      const MergeOperator* merge_operator, const Slice& key, PlainBaseValueTag,
      const Slice& value, const std::vector<Slice>& operands, Logger* logger,
      Statistics* statistics, SystemClock* clock, bool update_num_ops_stats,
      MergeOperator::OpFailureScope* op_failure_scope, ResultTs... results) {
    MergeOperator::MergeOperationInputV3::ExistingValue existing_value(value);

    return TimedFullMergeImpl(
        merge_operator, key, std::move(existing_value), operands, logger,
        statistics, clock, update_num_ops_stats, op_failure_scope, results...);
  }

  // 使用宽列基础值的合并
  template <typename... ResultTs>
  static Status TimedFullMerge(
      const MergeOperator* merge_operator, const Slice& key, WideBaseValueTag,
      const Slice& entity, const std::vector<Slice>& operands, Logger* logger,
      Statistics* statistics, SystemClock* clock, bool update_num_ops_stats,
      MergeOperator::OpFailureScope* op_failure_scope, ResultTs... results) {
    MergeOperator::MergeOperationInputV3::ExistingValue existing_value;

    Slice entity_copy(entity);
    WideColumns existing_columns;

    const Status s =
        WideColumnSerialization::Deserialize(entity_copy, existing_columns);
    if (!s.ok()) {
      return s;
    }

    existing_value = std::move(existing_columns);

    return TimedFullMergeImpl(
        merge_operator, key, std::move(existing_value), operands, logger,
        statistics, clock, update_num_ops_stats, op_failure_scope, results...);
  }

  // 使用预先存在的宽列基础值的合并
  template <typename... ResultTs>
  static Status TimedFullMerge(const MergeOperator* merge_operator,
                               const Slice& key, WideBaseValueTag,
                               const WideColumns& columns,
                               const std::vector<Slice>& operands,
                               Logger* logger, Statistics* statistics,
                               SystemClock* clock, bool update_num_ops_stats,
                               MergeOperator::OpFailureScope* op_failure_scope,
                               ResultTs... results) {
    MergeOperator::MergeOperationInputV3::ExistingValue existing_value(columns);

    return TimedFullMergeImpl(
        merge_operator, key, std::move(existing_value), operands, logger,
        statistics, clock, update_num_ops_stats, op_failure_scope, results...);
  }

  // 在压缩期间，合并条目直到遇到以下情况之一：
  // - 损坏的键
  // - Put/Delete 操作
  // - 不同的用户键
  // - 特定的序列号（快照边界）
  // - 从压缩过滤器返回的 REMOVE_AND_SKIP_UNTIL
  // 或 - 迭代结束
  //
  // 合并的结果可以通过 `MergeHelper::keys()` 和 `MergeHelper::values()` 访问，它们在下次调用 `MergeUntil()` 时无效。
  // `MergeOutputIterator` 专为迭代 `MergeHelper` 的最新一次 `MergeUntil()` 结果而设计。
  //
  // iter: (输入) 指向第一个合并类型条目
  //       (输出) 指向未包含在合并过程中的第一个条目
  // range_del_agg: (输入) 过滤被范围墓碑覆盖的合并操作数。
  // stop_before: (输入) 合并不应跨越的序列号。0 表示没有限制
  // at_bottom:   (输入) 如果迭代器覆盖底层级别，则为 true，这意味着我们可以达到此用户键的历史记录起点。
  // allow_data_in_errors: (输入) 如果为 true，错误/日志消息中将显示数据详细信息。
  // blob_fetcher: (输入) 用于压缩输入版本的 blob 提取器对象。
  // prefetch_buffers: (输入/输出) 用于压缩预读的 blob 文件预取缓冲区集合。
  // c_iter_stats: (输出) 压缩迭代统计信息。
  //
  // 返回以下状态之一：
  // - OK: 条目合并成功。
  // - MergeInProgress: 输出仅包含合并操作数。
  // - Corruption: 合并操作符报告合并失败或遇到损坏的键且未预期（仅适用于删除断言时）。
  // - ShutdownInProgress: 被关闭中断（*shutting_down == true）。
  //
  // 必须: 输入的第一个键未损坏。
  Status MergeUntil(InternalIterator* iter,
                    CompactionRangeDelAggregator* range_del_agg,
                    const SequenceNumber stop_before, const bool at_bottom,
                    const bool allow_data_in_errors,
                    const BlobFetcher* blob_fetcher,
                    const std::string* const full_history_ts_low,
                    PrefetchBufferCollection* prefetch_buffers,
                    CompactionIterationStats* c_iter_stats);

  // 使用构造函数中指定的压缩过滤器过滤合并操作数。返回过滤器的决策。
  // 使用 compaction_filter_value_ 和 compaction_filter_skip_until_ 作为压缩过滤器的可选输出。
  // user_key 包含时间戳（如果启用了用户定义的时间戳）。
  CompactionFilter::Decision FilterMerge(const Slice& user_key,
                                         const Slice& value_slice);

  // 查询合并结果
  // 这些结果在下一次调用 MergeUntil 前有效
  // 如果合并成功:
  //   - keys() 包含具有合并最新序列号的单个元素。类型将是 Put 或 Merge。见下面的 "重要提示 1"。
  //   - values() 包含将所有操作数合并在一起的单个元素的结果
  //
  //   重要提示 1: 键类型可能会在 MergeUntil 调用后更改。
  //        Put/Delete + Merge + ... + Merge => Put
  //        Merge + ... + Merge => Merge
  //
  // 如果合并操作符不是关联的，并且未找到 Put/Delete，则合并将失败。在这种情况下:
  //   - keys() 包含按迭代顺序看到的内部键列表。
  //   - values() 包含按相同顺序看到的值（合并）列表。
  //              values() 与 keys() 并行，因此 keys()
//              的第一个条目与 values() 的第一个条目对应，依此类推。这些列表将具有相同的长度。
//              所有这些对将是同一个用户键的合并。
//              见下面的 "重要提示 2"。
//   重要提示 2: 条目是从后向前按顺序遍历的。
//                因此，keys().back() 是迭代器看到的第一个键。
  const std::deque<std::string>& keys() const { return keys_; }
  const std::vector<Slice>& values() const {
    return merge_context_.GetOperands();
  }
  uint64_t TotalFilterTime() const { return total_filter_time_; }
  bool HasOperator() const { return user_merge_operator_ != nullptr; }

  // 如果压缩过滤器返回 REMOVE_AND_SKIP_UNTIL，则此方法将返回 true 并将 *until 填充为应跳过的键。
  // 如果返回 true，则 keys() 和 values() 为空。
  bool FilteredUntil(Slice* skip_until) const {
    if (!has_compaction_filter_skip_until_) {
      return false;
    }
    assert(compaction_filter_ != nullptr);
    assert(skip_until != nullptr);
    assert(compaction_filter_skip_until_.Valid());
    *skip_until = compaction_filter_skip_until_.Encode();
    return true;
  }

 private:
  Env* env_;
  SystemClock* clock_;
  const Comparator* user_comparator_;
  const MergeOperator* user_merge_operator_;
  const CompactionFilter* compaction_filter_;
  const std::atomic<bool>* shutting_down_;
  Logger* logger_;
  bool assert_valid_internal_key_;  // 强制执行无内部键损坏？
  bool allow_single_operand_;
  SequenceNumber latest_snapshot_;
  const SnapshotChecker* const snapshot_checker_;
  int level_;

  // 用于存储 MergeUntil 结果的临时区域
  // 在下一次 MergeUntil 调用前有效

  // 跟踪看到的键序列
  std::deque<std::string> keys_;
  // 与 keys_ 并行；存储操作数
  mutable MergeContext merge_context_;

  StopWatchNano filter_timer_;
  uint64_t total_filter_time_;
  Statistics* stats_;

  bool has_compaction_filter_skip_until_ = false;
  std::string compaction_filter_value_;
  InternalKey compaction_filter_skip_until_;

  bool IsShuttingDown() {
    // 这是一个尽力而为的设施，因此 memory_order_relaxed 足够了。
    return shutting_down_ && shutting_down_->load(std::memory_order_relaxed);
  }

  template <typename Visitor>
  static Status TimedFullMergeCommonImpl(
      const MergeOperator* merge_operator, const Slice& key,
      MergeOperator::MergeOperationInputV3::ExistingValue&& existing_value,
      const std::vector<Slice>& operands, Logger* logger,
      Statistics* statistics, SystemClock* clock, bool update_num_ops_stats,
      MergeOperator::OpFailureScope* op_failure_scope, Visitor&& visitor);

  // 变体，直接暴露合并结果（对于宽列以序列化形式）及其值类型。用于迭代器和压缩。
  static Status TimedFullMergeImpl(
      const MergeOperator* merge_operator, const Slice& key,
      MergeOperator::MergeOperationInputV3::ExistingValue&& existing_value,
      const std::vector<Slice>& operands, Logger* logger,
      Statistics* statistics, SystemClock* clock, bool update_num_ops_stats,
      MergeOperator::OpFailureScope* op_failure_scope, std::string* result,
      Slice* result_operand, ValueType* result_type);

  // 变体，将合并结果转换为客户端请求的形式。（例如，如果结果是宽列结构但客户端请求的是普通值形式，则返回默认列的值。）用于点查找。
  static Status TimedFullMergeImpl(
      const MergeOperator* merge_operator, const Slice& key,
      MergeOperator::MergeOperationInputV3::ExistingValue&& existing_value,
      const std::vector<Slice>& operands, Logger* logger,
      Statistics* statistics, SystemClock* clock, bool update_num_ops_stats,
      MergeOperator::OpFailureScope* op_failure_scope,
      std::string* result_value, PinnableWideColumns* result_entity);
};

// MergeOutputIterator 可用于迭代合并的结果。
class MergeOutputIterator {
 public:
  // MergeOutputIterator 绑定到 MergeHelper 实例。
  explicit MergeOutputIterator(const MergeHelper* merge_helper);

  // 定位到输出中的第一个记录。
  void SeekToFirst();
  // 前进到输出中的下一个记录。
  void Next();

  Slice key() { return Slice(*it_keys_); }
  Slice value() { return Slice(*it_values_); }
  bool Valid() { return it_keys_ != merge_helper_->keys().rend(); }

 private:
  const MergeHelper* merge_helper_;
  std::deque<std::string>::const_reverse_iterator it_keys_;
  std::vector<Slice>::const_reverse_iterator it_values_;
};

}  // namespace ROCKSDB_NAMESPACE
