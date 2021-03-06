// Copyright (c) 2011 The LevelDB Authors. All rights reserved.
// Use of this source code is governed by a BSD-style license that can be
// found in the LICENSE file. See the AUTHORS file for names of contributors.

#include "table/format.h"

#include "leveldb/env.h"
#include "port/port.h"
#include "table/block.h"
#include "util/coding.h"
#include "util/crc32c.h"

namespace leveldb {

void BlockHandle::EncodeTo(std::string* dst) const {
  // Sanity check that all fields have been set
  assert(offset_ != ~static_cast<uint64_t>(0)); // 确保值有效
  assert(size_ != ~static_cast<uint64_t>(0));
  // 编码一下
  PutVarint64(dst, offset_);
  PutVarint64(dst, size_);
}

Status BlockHandle::DecodeFrom(Slice* input) {
  if (GetVarint64(input, &offset_) &&
      GetVarint64(input, &size_)) {
    return Status::OK();
  } else {
    return Status::Corruption("bad block handle");
  }
}


// 这个函数的作用并不只是把Footer编码出来。而是把FOOTER编码输出到
// 给定的内存缓冲区dst里面。
// 所以后面在assert的时候，最后生成的dst的大小，相比原来的数据区的大小
// 是要多出48bytes的。
void Footer::EncodeTo(std::string* dst) const {
  const size_t original_size = dst->size();
  // 先把meta block index index解析出来
  metaindex_handle_.EncodeTo(dst); // 这里最后实际上是采用了dst的append操作。
  // 再把data block index index解析出来
  index_handle_.EncodeTo(dst);

  // 前面的两个handle每个都需要20bytes，所以前面两个加在一起就需要40bytes.
  // 这个时候看一下需要填充多少个空格
  dst->resize(2 * BlockHandle::kMaxEncodedLength);  // Padding
  // 接着把sstable里面的magic number填写进去
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber & 0xffffffffu));
  PutFixed32(dst, static_cast<uint32_t>(kTableMagicNumber >> 32));

  assert(dst->size() == original_size + kEncodedLength);
  (void)original_size;  // Disable unused variable warning.
}

Status Footer::DecodeFrom(Slice* input) {
  // 首先不要花时间去解析什么meta block index index/data block index index
  // 则是检查一下magic number是不是一样的。
  const char* magic_ptr = input->data() + kEncodedLength - 8;
  const uint32_t magic_lo = DecodeFixed32(magic_ptr);
  const uint32_t magic_hi = DecodeFixed32(magic_ptr + 4);
  const uint64_t magic = ((static_cast<uint64_t>(magic_hi) << 32) |
                          (static_cast<uint64_t>(magic_lo)));
  // 如果magic number不相等，那么直接报错
  if (magic != kTableMagicNumber) {
    return Status::Corruption("not an sstable (bad magic number)");
  }

  // 解析meta block index index
  Status result = metaindex_handle_.DecodeFrom(input);
  if (result.ok()) {
    // 解析data block index index
    result = index_handle_.DecodeFrom(input);
  }
  if (result.ok()) {
    // We skip over any leftover data (just padding for now) in "input"
    // end指向footer的尾部
    const char* end = magic_ptr + 8;
    // input里面放什么?
    // 这里应该是解析完之后，移动slice.
    *input = Slice(end, input->data() + input->size() - end);
  }
  return result;
}

// 从file中读取一个block?
// handle指明了偏移的位置和大小
// 把读取的结果放到BlockContent即result里面
// 注意：在ReadBlock里面生成的内存，会通过设置
// result->cacheable变量
// 把这个内存传递给Block
// 如果Block发现这个内存是需要自己释放的
// 那么在Block的析构函数里面就释放掉了
Status ReadBlock(RandomAccessFile* file,     // 随机访问文件
                 const ReadOptions& options, // 读选项
                 const BlockHandle& handle,  // 指向要读取的数据的位置
                 BlockContents* result) {    // 读到哪里去
  // 清空掉原来的result.
  result->data = Slice();
  // 默认不cache
  result->cachable = false;
  // 调用者需要负责释放内存?
  result->heap_allocated = false;

  // Read the block contents as well as the type/crc footer.
  // See table_builder.cc for the code that built this structure.
  // 拿到block的size
  size_t n = static_cast<size_t>(handle.size());
  // 总的内存区域
  // 这个是在函数内部申请好
  // 到时候谁来释放呢？
  // 每个block后面都是有个5bytes的尾巴
  // 这里没有向Arena申请内存
  char* buf = new char[n + kBlockTrailerSize];

  Slice contents;
  // 读出来，放到contents里面
  // 真正的读取操作，对于posiz而言，则是在
  // env_posix.cc  virtual Status Read(uint64_t offset, size_t n, Slice* result,
  // 这里直接调用了pread/pwrite
  Status s = file->Read(handle.offset(), n + kBlockTrailerSize, &contents, buf);
  if (!s.ok()) {
    delete[] buf;
    return s;
  }
  // 如果读出来的大小与想要的大小不一样
  if (contents.size() != n + kBlockTrailerSize) {
    delete[] buf;
    return Status::Corruption("truncated block read");
  }

  // Check the crc of the type and the block contents
  const char* data = contents.data();    // Pointer to where Read put the data
  // 看一下是不是需要进行crc校验
  if (options.verify_checksums) {
    const uint32_t crc = crc32c::Unmask(DecodeFixed32(data + n + 1));
    const uint32_t actual = crc32c::Value(data, n + 1);
    if (actual != crc) {
      delete[] buf;
      s = Status::Corruption("block checksum mismatch");
      return s;
    }
  }

  // 看一下是否需要压缩，或者是解压缩
  switch (data[n]) {
    case kNoCompression:
      if (data != buf) {
        // 如果contents里面是自带内存的
        // 那么就没有必要使用这个函数内部申请的buf
        // 所以把buf清空掉
        // File implementation gave us pointer to some other data.
        // Use it directly under the assumption that it will be live
        // while the file is open.
        // 在正常情况下，都是通过把数据读入到buf里面，然后通过buf构造出
        // Slice，但是，偶尔有例外，就是当
        // 文件源是一种memmap的文件的时候
        // content会直接从内存源数据上构造出来。
        // 这个时候，buf就会显得多余。
        // 由于是引用的别的地方的内存
        // 所以是否可以加入缓存，是否可以被析构都是false
        // 因为这个内存的生命周期都是不归content管的。
        delete[] buf;
        result->data = Slice(data, n);
        result->heap_allocated = false;
        result->cachable = false;  // Do not double-cache
      } else {
        // 如果contents里面使用了新生成的buf
        // 那么就需要自己去释放内存
        result->data = Slice(buf, n);
        result->heap_allocated = true;
        result->cachable = true;
      }

      // Ok
      break;
    case kSnappyCompression: {
      size_t ulength = 0;
      if (!port::Snappy_GetUncompressedLength(data, n, &ulength)) {
        delete[] buf;
        return Status::Corruption("corrupted compressed block contents");
      }
      // 取得解压缩之后的结果
      char* ubuf = new char[ulength];
      if (!port::Snappy_Uncompress(data, n, ubuf)) {
        delete[] buf;
        delete[] ubuf;
        return Status::Corruption("corrupted compressed block contents");
      }
      // 删除压缩的内容
      delete[] buf;
      // 由于使用了新的memory
      // 所以需要caller来释放
      result->data = Slice(ubuf, ulength);
      result->heap_allocated = true;
      result->cachable = true;
      break;
    }
    default:
      delete[] buf;
      return Status::Corruption("bad block type");
  }

  return Status::OK();
}

}  // namespace leveldb
