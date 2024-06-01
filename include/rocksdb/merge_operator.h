// 版权所有（c）2011至今，Facebook，Inc。 保留所有权利。
// 本源代码受GPLv2（在根目录中的COPYING文件中找到）和Apache 2.0许可证
// （在根目录中的LICENSE.Apache文件中找到）的双重许可证

#pragma once

#include <deque>
#include <memory>
#include <string>
#include <utility>
#include <variant>
#include <vector>

#include "rocksdb/customizable.h"
#include "rocksdb/slice.h"
#include "rocksdb/wide_columns.h"

namespace ROCKSDB_NAMESPACE {

class Slice;
class Logger;

// 合并操作符
//
// 本质上，合并操作符指定了合并的语义，这仅客户端知道。它可以是数字添加、列表追加、字符串连接、编辑数据结构等等，任何内容。
// 另一方面，库关心的是在正确的时间（在获取、迭代、压实期间等）对此接口进行操作。
//
// 要使用合并，客户端需要提供一个实现以下接口之一的对象：
//  a) AssociativeMergeOperator - 用于大多数简单语义（始终获取两个值，并将它们合并为一个值，然后将其放回rocksdb）；数字添加和字符串连接是示例；
//
//  b) MergeOperator - 用于所有更抽象/复杂操作的通用类；一个方法（FullMergeV3）用于将Put/Delete值与合并操作数合并；另一个方法（PartialMerge）用于将多个操作数合并在一起。如果您的键值具有复杂结构但仍希望支持客户端特定的增量更新，则这尤其有用。
//
// AssociativeMergeOperator更容易实现。MergeOperator更强大。
//
// 有关更多详细信息和示例实现，请参阅rocksdb-merge wiki。
//
// 异常绝对不得传播出覆盖的函数进入RocksDB，因为RocksDB不具有异常安全性。这可能导致未定义的行为，包括数据丢失、未报告的损坏、死锁等等。
class MergeOperator : public Customizable {
 public:
  virtual ~MergeOperator() {}
  static const char* Type() { return "MergeOperator"; }
  static Status CreateFromString(const ConfigOptions& opts,
                                 const std::string& id,
                                 std::shared_ptr<MergeOperator>* result);

  // 给客户端提供一种表达读->修改->写语义的方法
  // key:          (IN) 与此合并操作关联的键。如果键空间被分区，并且不同的子空间引用具有不同合并操作语义的不同类型的数据，则客户端可以基于它复用合并操作符
  // existing:  (IN)  null表示此操作之前键不存在
  // operand_list:(IN) 要应用的合并操作的序列，front()优先。
  // new_value:(OUT)   客户端负责在此处填充合并结果。
  // new_value指向的字符串将为空。
  // logger:   (IN)   客户端可以在合并期间使用此选项记录错误。
  //
  // 返回true表示成功。
  // 传递的所有值都将是特定于客户端的值。因此，如果此方法返回false，则是因为客户端指定了错误的数据或存在内部损坏。库将将其视为错误处理。
  //
  // 也使用*logger记录错误消息。
  virtual bool FullMerge(const Slice& /*key*/, const Slice* /*existing_value*/,
                         const std::deque<std::string>& /*operand_list*/,
                         std::string* /*new_value*/, Logger* /*logger*/) const {
    // 不推荐使用，请使用FullMergeV2()
    assert(false);
    return false;
  }

  struct MergeOperationInput {
    // 如果启用了用户定义的时间戳，则`_key`包括时间戳。
    explicit MergeOperationInput(const Slice& _key,
                                 const Slice* _existing_value,
                                 const std::vector<Slice>& _operand_list,
                                 Logger* _logger)
        : key(_key),
          existing_value(_existing_value),
          operand_list(_operand_list),
          logger(_logger) {}

    // 与合并操作关联的键。
    const Slice& key;
    // 当前键的现有值，nullptr表示该值在此之前不存在。
    const Slice* existing_value;
    // 要应用的操作数列表。
    const std::vector<Slice>& operand_list;
    // 客户端在合并操作期间记录任何错误的logger。
    Logger* logger;
  };

  enum class OpFailureScope {
    kDefault,
    kTryMerge,
    kMustMerge,
    kOpFailureScopeMax,
  };

  struct MergeOperationOutput {
    explicit MergeOperationOutput(std::string& _new_value,
                                  Slice& _existing_operand)
        : new_value(_new_value), existing_operand(_existing_operand) {}

    // 客户端负责在此处填充合并结果。
    std::string& new_value;
    // 如果合并结果是现有操作数之一（或现有值），客户端可以将此字段设置为操作数（或现有值），而不是使用new_value。
    Slice& existing_operand;
    // 指示故障的波及范围。仅在从填充`MergeOperationOutput`的API中返回`false`时才有意义。
    // 当返回`false`时，RocksDB操作将以下列方式处理这些值：
    //
    // - `OpFailureScope::kDefault`：退回到默认值（`OpFailureScope::kTryMerge`）
    // - `OpFailureScope::kTryMerge`：尝试合并该键的操作将失败。这包括将DB置于只读模式的刷新和压实。
    // - `OpFailureScope::kMustMerge`：必须合并该键的操作将失败（例如，`Get()`、`MultiGet()`、迭代）。刷新/压实仍然可以通过将原始输入操作数复制到输出来进行。
    OpFailureScope op_failure_scope = OpFailureScope::kDefault;
  };

  // 这个函数按照时间顺序在现有值的基础上应用一堆合并操作。此方法有两种使用方式：
  // a) 在Get()操作期间，用于计算键的最终值
  // b) 在压实期间，为了将一些操作数与基础值折叠在一起。
  //
  // 注意：方法的名称有点误导，因为无论在Get()还是压实的情况下，它都可能被调用在操作数的子集上：
  // K:    0    +1    +2    +7    +4     +5      2     +1     +2
  //                              ^
  //                              |
  //                          快照
  // 在上面的示例中，Get(K)操作将使用基值为2和操作数[+1，+2]调用FullMerge。压实过程可能决定通过使用基值为0和操作数[+1，+2，+7，+4]来折叠历史的开始，从而执行完整的Merge。
  virtual bool FullMergeV2(const MergeOperationInput& merge_in,
                           MergeOperationOutput* merge_out) const;

  struct MergeOperationInputV3 {
    using ExistingValue = std::variant<std::monostate, Slice, WideColumns>;
    using OperandList = std::vector<Slice>;

    explicit MergeOperationInputV3(const Slice& _key,
                                   ExistingValue&& _existing_value,
                                   const OperandList& _operand_list,
                                   Logger* _logger)
        : key(_key),
          existing_value(std::move(_existing_value)),
          operand_list(_operand_list),
          logger(_logger) {}

    // 用户键，包括用户定义的时间戳（如果适用）。
    const Slice& key;
    // 合并操作的基值。可以是三种情况之一（请参见上面的ExistingValue变体）：没有现有值，纯现有值或宽列现有值。
    ExistingValue existing_value;
    // 要应用的操作数列表。
    const OperandList& operand_list;
    // 在合并操作期间发生故障时使用的日志记录器。
    Logger* logger;
  };

  struct MergeOperationOutputV3 {
    using NewColumns = std::vector<std::pair<std::string, std::string>>;
    using NewValue = std::variant<std::string, NewColumns, Slice>;

    // 合并操作的结果。可以是三种情况之一（请参见上面的NewValue变体）：新的纯值，新的宽列值或现有的合并操作数。
    NewValue new_value;
    // 如果适用，故障的作用域。更多细节见上文。
    OpFailureScope op_failure_scope = OpFailureScope::kDefault;
  };

  // 支持在输入和输出端上使用宽列的FullMergeV3()的扩展版本，使应用能够在合并期间执行通用转换。为了向后兼容，默认实现调用FullMergeV2()。具体来说，如果没有基值或基值是键值，则默认实现将回退到FullMergeV2()。如果基值是宽列实体，则默认实现将调用FullMergeV2()来对默认列执行合并，并保持其他列不变。
  virtual bool FullMergeV3(const MergeOperationInputV3& merge_in,
                           MergeOperationOutputV3* merge_out) const;

  // 当两个操作数本身都是您将在相同顺序传递给DB::Merge()调用的合并操作类型时，此函数执行merge(left_op，right_op)。
  // （即：DB::Merge(key，left_op)，然后是DB::Merge(key，right_op)）。
  //
  // PartialMerge应该将它们组合成一个合并操作，保存到*new_value中，然后返回true。
  // *new_value应构造成调用DB::Merge(key，*new_value)将产生与调用DB::Merge(key，left_op)然后调用DB::Merge(key，right_op)相同的结果。
  //
  // new_value指向的字符串将为空。
  //
  // PartialMergeMulti的默认实现将使用此函数作为帮助器，以确保向后兼容。MergeOperator的任何后继类都应该实现PartialMerge或PartialMergeMulti，尽管建议实现PartialMergeMulti，因为一次合并多个操作数通常比一次合并两个操作数更有效。
  //
  // 如果无法或不可行地合并两个操作，则将new_value保持不变，并返回false。库将在看到基值（Put/Delete/数据库末尾）后，内部跟踪操作，并按正确的顺序应用它们。
  //
  // 注意：目前没有办法区分错误/损坏和简单的“返回false”。暂时，客户端应该在无法执行部分合并的任何情况下返回false，无论原因如何。如果数据存在损坏，请在FullMergeV3()函数中处理它并在那里返回false。PartialMerge的默认实现将始终返回false。
  virtual bool PartialMerge(const Slice& /*key*/, const Slice& /*left_operand*/,
                            const Slice& /*right_operand*/,
                            std::string* /*new_value*/,
                            Logger* /*logger*/) const {
    return false;
  }

  // 当所有操作数本身都是您将在相同顺序传递给DB::Merge()调用的合并操作类型时，此函数执行合并。
  // （即DB::Merge(key，operand_list[0])，然后是DB::Merge(key，operand_list[1])，...）
  //
  // PartialMergeMulti应将它们组合成一个合并操作，保存到*new_value中，然后返回true。*new_value应构造成调用DB::Merge(key，*new_value)将产生与为operand_list中的每个操作数依次调用DB::Merge(key，operand)相同的结果。
  //
  // new_value指向的字符串将为空。
  //
  // 当存在至少两个操作数时，将调用PartialMergeMulti函数。
  //
  // 在默认实现中，PartialMergeMulti将多次调用PartialMerge，其中每次仅合并两个操作数。开
  // 发者应该实现PartialMergeMulti，或者实现PartialMerge作为默认PartialMergeMulti的辅助函数。
  virtual bool PartialMergeMulti(const Slice& key,
                                 const std::deque<Slice>& operand_list,
                                 std::string* new_value, Logger* logger) const;

  // MergeOperator的名称。用于检查MergeOperator是否不匹配（即，使用一个MergeOperator创建的DB是否使用不同的MergeOperator访问）
  // TODO：当前名称未持久存储，因此没有强制执行检查。客户端负责在DB打开之间提供一致的MergeOperator。
  const char* Name() const override = 0;

  // 确定是否可以只使用单个合并操作数调用PartialMerge。
  // 覆盖并返回true以允许单个操作数。PartialMerge和PartialMergeMulti应该被覆盖和正确实现以正确处理单个操作数。
  virtual bool AllowSingleOperand() const { return false; }

  // 允许控制在Get期间何时调用完整合并。
  // 这可以用于限制在点查找期间查看的合并操作数的数量，从而有助于限制从中读取的级别数量。
  // 不适用于迭代器。
  //
  // 注意：出于性能原因，合并操作数以与它们合并的方式相对于它们合并的顺序相反的顺序传递给此函数，请参见：
  // https://github.com/facebook/rocksdb/issues/3865
  virtual bool ShouldMerge(const std::vector<Slice>& /*operands*/) const {
    return false;
  }
};

// 更简单的，关联的合并操作符。
class AssociativeMergeOperator : public MergeOperator {
 public:
  ~AssociativeMergeOperator() override {}

  // 给客户端提供一种表达读->修改->写语义的方法
  // key:           (IN) 与此合并操作关联的键。
  // existing_value:(IN) null表示在此操作之前键不存在
  // value:         (IN) 要更新/合并existing_value的值
  // new_value:    (OUT) 客户端负责在此处填充合并结果。new_value指向的字符串将为空。
  // logger:        (IN) 客户端可以在合并期间使用此选项记录错误。
  //
  // 返回true表示成功。
  // 传递的所有值都将是客户端特定的值。因此，如果此方法返回false，则是因为客户端指定了错误的数据或存在内部损坏。客户端应假设库会将其视为错误处理。
  virtual bool Merge(const Slice& key, const Slice* existing_value,
                     const Slice& value, std::string* new_value,
                     Logger* logger) const = 0;

 private:
  // MergeOperator函数的默认实现
  bool FullMergeV2(const MergeOperationInput& merge_in,
                   MergeOperationOutput* merge_out) const override;

  bool PartialMerge(const Slice& key, const Slice& left_operand,
                    const Slice& right_operand, std::string* new_value,
                    Logger* logger) const override;
};

}  // namespace ROCKSDB_NAMESPACE
