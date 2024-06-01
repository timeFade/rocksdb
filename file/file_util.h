// 版权声明
// 包含两个许可证的声明：GPLv2 和 Apache 2.0
//
#pragma once
#include <string>

#include "file/filename.h"  // 文件名相关的函数
#include "options/db_options.h"  // 数据库选项
#include "rocksdb/env.h"  // RocksDB 环境
#include "rocksdb/file_system.h"  // RocksDB 文件系统
#include "rocksdb/sst_file_writer.h"  // SST 文件写入器
#include "rocksdb/statistics.h"  // 统计信息
#include "rocksdb/status.h"  // 状态信息
#include "rocksdb/system_clock.h"  // 系统时钟
#include "rocksdb/types.h"  // RocksDB 类型
#include "trace_replay/io_tracer.h"  // IO 追踪器

namespace ROCKSDB_NAMESPACE {
// 对文件系统进行操作和管理的一些常用功能
// 复制文件到目标位置
IOStatus CopyFile(FileSystem* fs, const std::string& source,
                  Temperature src_temp_hint,
                  std::unique_ptr<WritableFileWriter>& dest_writer,
                  uint64_t size, bool use_fsync,
                  const std::shared_ptr<IOTracer>& io_tracer);

// 复制文件到目标位置
IOStatus CopyFile(FileSystem* fs, const std::string& source,
                  Temperature src_temp_hint, const std::string& destination,
                  Temperature dst_temp, uint64_t size, bool use_fsync,
                  const std::shared_ptr<IOTracer>& io_tracer);

// 生成文件校验和
IOStatus GenerateOneFileChecksum(
    FileSystem* fs, const std::string& file_path,
    FileChecksumGenFactory* checksum_factory,
    const std::string& requested_checksum_func_name, std::string* file_checksum,
    std::string* file_checksum_func_name,
    size_t verify_checksums_readahead_size, bool allow_mmap_reads,
    std::shared_ptr<IOTracer>& io_tracer, RateLimiter* rate_limiter,
    const ReadOptions& read_options, Statistics* stats, SystemClock* clock);

// 根据读取选项准备 IO 选项
IOStatus PrepareIOFromReadOptions(const ReadOptions& ro,
                                  SystemClock* clock, IOOptions& opts);

// 根据写入选项准备 IO 选项
IOStatus PrepareIOFromWriteOptions(const WriteOptions& wo,
                                   IOOptions& opts);

// 删除数据库文件
Status DeleteDBFile(const ImmutableDBOptions* db_options,
                    const std::string& fname, const std::string& path_to_sync,
                    const bool force_bg, const bool force_fg);

// 创建文件
IOStatus CreateFile(FileSystem* fs, const std::string& destination,
                    const std::string& contents, bool use_fsync);

// 销毁目录及其内容（仅用于测试）
Status DestroyDir(Env* env, const std::string& dir);

// 检查文件系统支持的功能
inline bool CheckFSFeatureSupport(FileSystem* fs, FSSupportedOps feat);

}  // namespace ROCKSDB_NAMESPACE
