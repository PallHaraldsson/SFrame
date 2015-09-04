/**
 * Copyright (C) 2015 Dato, Inc.
 * All rights reserved.
 *
 * This software may be modified and distributed under the terms
 * of the BSD license. See the LICENSE file for details.
 */
#ifndef GRAPHLAB_FILEIO_CACHING_DEVICE_HPP
#define GRAPHLAB_FILEIO_CACHING_DEVICE_HPP
#include <logger/logger.hpp>
#include <fileio/block_cache.hpp>
#include <fileio/sanitize_url.hpp>
namespace graphlab {

// private namespace 
namespace {
const size_t READ_CACHING_BLOCK_SIZE = 64*1024*1024; // 64 MB
} // end private namespace 

/**
 * Can be wrapped around any device implement to provide read caching. This
 * should be used only when the filesystem we are accessing is rather remote.
 * It uses the \ref block_cache to cache large blocks on the cache:// file 
 * system.
 *
 * Before:
 * \code
 *   typedef boost::iostreams::stream<s3_device> s3_fstream;
 * \endcode
 *
 * After:
 * \code
 *   typedef boost::iostreams::stream<read_caching_device<s3_device> > s3_fstream;
 * \endcode
 *
 * It uses the \ref block_cache to pro
 */
template <typename T>
class read_caching_device {
 public: // boost iostream concepts
  typedef typename T::char_type char_type;
  typedef typename T::category category;

  read_caching_device() { }

  read_caching_device(const std::string& filename, const bool write = false) {
    m_filename = filename;
    m_contents = std::make_shared<T>(filename, write);
    m_file_size = m_contents->file_size();
    m_writing = write;
  }

  // Because the device has bidirectional tag, close will be called 
  // twice, one with the std::ios_base::in, followed by out.
  // Only close the file when the close tag matches the actual file type.
  void close(std::ios_base::openmode mode = std::ios_base::openmode()) {
    if (mode == std::ios_base::out && m_writing) {
      m_contents->close(mode);
      m_contents.reset();
      // evict all blocks for this key
      auto& bc = block_cache::get_instance();
      size_t block_number = 0;
      while(1) {
        std::string key = get_key_name(block_number);
        if (bc.evict_key(key) == false) break;
        ++ block_number;
      }
    } else if (mode == std::ios_base::in && !m_writing) {
      m_contents->close(mode);
      m_contents.reset();
    }
  }

  /** the optimal buffer size is 0. */
  inline std::streamsize optimal_buffer_size() const { return 0; }

  std::streamsize read(char* strm_ptr, std::streamsize n) {
    // there is an upper limit of how many bytes we can read
    // based on the file size
    n = std::min<std::streamsize>(n, m_file_size - m_file_pos);
    std::streamsize ret = 0;
    while(n > 0) {
      // the block number containing the offset.
      auto block_number = m_file_pos / READ_CACHING_BLOCK_SIZE;
      // the offset inside the block
      auto block_offset = m_file_pos % READ_CACHING_BLOCK_SIZE;
      // number of bytes I can read inside this block before I hit the next block
      size_t n_bytes = (block_number + 1) * READ_CACHING_BLOCK_SIZE - m_file_pos;
      n_bytes = std::min<size_t>(n_bytes, n);
      bool success = fetch_block(strm_ptr + ret,
                                 block_number,
                                 block_offset,
                                 n_bytes);
      if (success == false) {
        log_and_throw(std::string("Unable to read ") + m_filename);
      }
      n -= n_bytes;
      ret += n_bytes;
      // advance the file position
      m_file_pos += n_bytes;
    }
    return ret;
  }

  std::streamsize write(const char* strm_ptr, std::streamsize n) {
    m_contents->write(strm_ptr, n);
  }

  bool good() const {
    return m_contents->good();
  }

  /**
   * Seeks to a different location. 
   */
  std::streampos seek(std::streamoff off, 
                      std::ios_base::seekdir way, 
                      std::ios_base::openmode openmode) {
    if (openmode == std::ios_base::in) {
      if (way == std::ios_base::beg) {
        m_file_pos = std::min<std::streamoff>(off, m_file_size);
      } else if (way == std::ios_base::cur) {
        m_file_pos = std::min<std::streamoff>(m_file_pos + off, m_file_size);
        m_file_pos = std::max<std::streamoff>(m_file_pos, 0);
      } else if (way == std::ios_base::end) {
        m_file_pos = std::min<std::streamoff>(m_file_size + off - 1, m_file_size);
        m_file_pos = std::max<std::streamoff>(m_file_pos, 0);
      }
      return m_file_pos;
    } else {
      m_contents->seek(off, way, openmode);
    }
  }

  /**
   * Returns the file size of the opened file.
   * Returns (size_t)(-1) if there is no file opened, or if there is an
   * error obtaining the file size.
   */
  size_t file_size() const {
    return m_file_size;
  }

  /// Not supported
  std::shared_ptr<std::istream> get_underlying_stream() {
    return nullptr;
  }

 private:
  std::string m_filename;
  std::shared_ptr<T> m_contents;
  size_t m_file_size = 0;
  std::streamoff m_file_pos = 0;
  bool m_writing = true;

  std::string get_key_name(size_t block_number) {
    // we generate a key name that will never appear in any filename
    return  m_filename + "////:" + std::to_string(block_number);
  }
  /**
   * Fetches the contents of a block.
   * Returns true on success and false on failure.
   */
  bool fetch_block(char* output, 
                   size_t block_number,
                   size_t startpos, 
                   size_t length) {
    auto& bc = block_cache::get_instance();
    std::string key = get_key_name(block_number);
    int64_t ret = bc.read(key, output, startpos, startpos + length);
    if (ret == length) return true;

    logstream(LOG_INFO) << "Fetching " << sanitize_url(m_filename) << " Block " << block_number << std::endl;
    // ok. failure... no such block or block is bad. We read it ourselves.
    // read the whole block
    auto block_start = block_number * READ_CACHING_BLOCK_SIZE;
    auto block_end = std::min(block_start +  READ_CACHING_BLOCK_SIZE, m_file_size);
    // seek to the block and read the whole block at once
    m_contents->seek(block_start, std::ios_base::beg, std::ios_base::in);
    std::string block_contents(block_end - block_start, 0);
    auto bytes_read = m_contents->read(&(block_contents[0]), 
                                       block_end - block_start);
    // read failed.
    if (bytes_read < block_end - block_start) return false;

    // write the block
    bool write_block_ok = bc.write(key, block_contents);
    if (write_block_ok == false) {
      logstream(LOG_ERROR) << "Unable to write block " << key << std::endl;
      // still ok. we can continue. but too many of these are bad.
    }
    // since we just read the block, lets fill the output
    const char* src = block_contents.c_str();
    memcpy(output, src + startpos, length);
    return true;
  }

}; // end of read_caching_device

} // namespace graphlab
#endif
