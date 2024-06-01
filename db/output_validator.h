//  版权所有 (c) 2011-present, Facebook, Inc.  保留所有权利。
//  本源码依据 GPLv2（可在根目录的 COPYING 文件中找到）和 Apache 2.0 许可证（可在根目录的 LICENSE.Apache 文件中找到）获得许可。

#pragma once
#include "db/dbformat.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"

namespace ROCKSDB_NAMESPACE {

// 一个类，用于验证插入到 SST 文件中的键/值对。
// 使用 OutputValidator::Add() 传递文件的每个键/值对，
// 类将验证键的顺序，并可选择计算所有键和值的哈希值。
class OutputValidator {
 public:
  // 构造函数，接受内部键比较器、是否启用哈希计算以及预先计算的哈希值
  explicit OutputValidator(const InternalKeyComparator& icmp, bool enable_hash,
                           uint64_t precalculated_hash = 0)
      : icmp_(icmp),
        paranoid_hash_(precalculated_hash),
        enable_hash_(enable_hash) {}

  // 向 KV 序列添加一个键，并返回键是否符合标准，例如键是有序的。
  Status Add(const Slice& key, const Slice& value);

  // 比较两个键顺序是否相同。它可以用于比较插入文件中的键和读回的键。
  // 如果验证通过，则返回 true。
  bool CompareValidator(const OutputValidator& other_validator) {
    return GetHash() == other_validator.GetHash();
  }

  // 当前未打算持久化，因此在版本之间可能会有变更。
  uint64_t GetHash() const { return paranoid_hash_; }

 private:
  const InternalKeyComparator& icmp_;  // 内部键比较器
  std::string prev_key_;  // 上一个键
  uint64_t paranoid_hash_ = 0;  // 偏执哈希值
  bool enable_hash_;  // 是否启用哈希计算
};

}  // namespace ROCKSDB_NAMESPACE
