//  Copyright (c) 2011-present, Facebook, Inc.  All rights reserved.
//  This source code is licensed under both the GPLv2 (found in the
//  COPYING file in the root directory) and Apache 2.0 License
//  (found in the LICENSE.Apache file in the root directory).
//
// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.
//
// File names used by DB code

#pragma once

#include <stdint.h>

#include <string>
#include <unordered_map>
#include <vector>

#include "options/db_options.h"
#include "port/port.h"
#include "rocksdb/file_system.h"
#include "rocksdb/options.h"
#include "rocksdb/slice.h"
#include "rocksdb/status.h"
#include "rocksdb/transaction_log.h"

namespace ROCKSDB_NAMESPACE {

class Env;
class Directory;
class SystemClock;
class WritableFileWriter;

#ifdef OS_WIN
constexpr char kFilePathSeparator = '\\';  // 文件路径分隔符在Windows下为反斜杠'\'
#else
constexpr char kFilePathSeparator = '/';   // 在其他系统（如Unix/Linux）下为正斜杠'/'
#endif

// 返回指定编号的数据库文件的日志文件名，以指定的数据库名为前缀
std::string LogFileName(const std::string& dbname, uint64_t number);

// 返回指定编号的日志文件名
std::string LogFileName(uint64_t number);

// 返回指定编号的 Blob 文件名
std::string BlobFileName(uint64_t number);

// 返回指定 Blob 目录下指定编号的 Blob 文件名
std::string BlobFileName(const std::string& bdirname, uint64_t number);

// 返回指定数据库名和 Blob 目录下指定编号的 Blob 文件名
std::string BlobFileName(const std::string& dbname, const std::string& blob_dir,
                         uint64_t number);

// 返回指定数据库名的归档目录名
std::string ArchivalDirectory(const std::string& dbname);

// 返回指定编号的归档日志文件名，以指定的数据库名为前缀
std::string ArchivedLogFileName(const std::string& dbname, uint64_t num);

// 根据指定名称和编号创建 Table 文件名
std::string MakeTableFileName(const std::string& name, uint64_t number);

// 根据指定编号创建 Table 文件名
std::string MakeTableFileName(uint64_t number);

// 返回 RocksDB 格式的 SSTable 文件名，用于与 LevelDB 的文件名进行转换
std::string Rocks2LevelTableFileName(const std::string& fullname);

// 将 Table 文件名解析为编号
uint64_t TableFileNameToNumber(const std::string& name);

// 返回指定编号的数据库表文件名，以指定的数据库路径集合为前缀
std::string TableFileName(const std::vector<DbPath>& db_paths, uint64_t number,
                          uint32_t path_id);

// 格式化文件编号，用于文件名中
void FormatFileNumber(uint64_t number, uint32_t path_id, char* out_buf,
                      size_t out_buf_size);

// 返回指定编号的描述符文件名，以指定的数据库名为前缀
std::string DescriptorFileName(const std::string& dbname, uint64_t number);

// 返回指定编号的描述符文件名
std::string DescriptorFileName(uint64_t number);

extern const std::string kCurrentFileName;  // 当前文件名为 "CURRENT"

// 返回当前文件的文件名，该文件包含当前清单文件的名称，以指定的数据库名为前缀
std::string CurrentFileName(const std::string& dbname);

// 返回指定数据库名的锁文件名
std::string LockFileName(const std::string& dbname);

// 返回指定数据库名的临时文件名，以及指定的编号
std::string TempFileName(const std::string& dbname, uint64_t number);

// InfoLogPrefix 的辅助结构，用于日志文件名前缀
struct InfoLogPrefix {
  char buf[260];  // 前缀缓冲区
  Slice prefix;   // 前缀片段
  // 根据数据库的绝对路径编码，构造带有绝对路径的前缀
  explicit InfoLogPrefix(bool has_log_dir, const std::string& db_absolute_path);
  // 默认的前缀
  explicit InfoLogPrefix();
};

// 返回指定数据库名的信息日志文件名，可指定数据库路径和日志目录
std::string InfoLogFileName(const std::string& dbname,
                            const std::string& db_path = "",
                            const std::string& log_dir = "");

// 返回指定数据库名和时间戳的旧信息日志文件名，可指定数据库路径和日志目录
std::string OldInfoLogFileName(const std::string& dbname, uint64_t ts,
                               const std::string& db_path = "",
                               const std::string& log_dir = "");

extern const std::string kOptionsFileNamePrefix;  // 选项文件名前缀为 "OPTIONS-"
extern const std::string kTempFileNameSuffix;     // 临时文件名后缀为 "dbtmp"

// 返回给定数据库名和文件编号的选项文件名
std::string OptionsFileName(const std::string& dbname, uint64_t file_num);
std::string OptionsFileName(uint64_t file_num);

// 返回给定数据库名和文件编号的临时选项文件名
std::string TempOptionsFileName(const std::string& dbname, uint64_t file_num);

// 返回元数据库的名称，以指定的数据库名为前缀
std::string MetaDatabaseName(const std::string& dbname, uint64_t number);

// 返回 Identity 文件名，存储数据库的唯一编号，如果数据库丢失数据并重新创建，该编号将重新生成
std::string IdentityFileName(const std::string& dbname);

// 如果文件名是 RocksDB 文件，则解析其类型和编号
bool ParseFileName(const std::string& filename, uint64_t* number,
                   const Slice& info_log_name_prefix, FileType* type,
                   WalFileType* log_type = nullptr);
// 不包括日志文件
bool ParseFileName(const std::string& filename, uint64_t* number,
                   FileType* type, WalFileType* log_type = nullptr);

// 设置当前文件，使其指向指定编号的描述符文件
IOStatus SetCurrentFile(const WriteOptions& write_options, FileSystem* fs,
                        const std::string& dbname
                        uint64_t descriptor_number,
                        FSDirectory* dir_contains_current_file);

// 为数据库创建 Identity 文件
Status SetIdentityFile(const WriteOptions& write_options, Env* env,
                       const std::string& dbname,
                       const std::string& db_id = {});

// 同步清单文件 `file`
IOStatus SyncManifest(const ImmutableDBOptions* db_options,
                      const WriteOptions& write_options,
                      WritableFileWriter* file);

// 返回信息日志文件名列表，只包含文件名，父目录名存储在 `parent_dir` 中
// `db_log_dir` 应与 `options.db_log_dir` 相同
Status GetInfoLogFiles(const std::shared_ptr<FileSystem>& fs,
                       const std::string& db_log_dir, const std::string& dbname,
                       std::string* parent_dir,
                       std::vector<std::string>* file_names);

// 标准化路径字符串，确保其格式正确
std::string NormalizePath(const std::string& path);
}  // namespace ROCKSDB_NAMESPACE
