#ifndef __INDEX_HPP__
#define __INDEX_HPP__

#include <algorithm>
#include <cstddef>
#include <fstream>
#include <iostream>
#include <iterator>

#include <assert.h>

#include "boost/interprocess/file_mapping.hpp"
#include "boost/interprocess/mapped_region.hpp"
#include "boost/optional.hpp"

class LineIndex {
private:
  const std::size_t line_count_;
  boost::interprocess::file_mapping mapping_;
  boost::interprocess::mapped_region region_;
  std::size_t *const line_starts_;

  static const char *create_file(const char *name, std::size_t size) {
    std::ofstream ofs(name, std::ios::binary | std::ios::out);
    ofs.seekp(size - 1);
    ofs.write("", 1);
    return name;
  }

public:
  template <typename It>
  LineIndex(It first, It last, const char *filename)
      : line_count_(std::count(first, last, '\n')),
        mapping_(create_file(filename, total_byte_size()),
                 boost::interprocess::read_write),
        region_(mapping_, boost::interprocess::read_write, 0,
                total_byte_size()),
        line_starts_(reinterpret_cast<size_t *>(region_.get_address())) {

    std::size_t last_start = 0;
    for (std::size_t n = 0; n < line_count_; ++n) {
      auto eol = std::find(first, last, '\n');
      if (eol == last) {
        break;
      }
      last_start = line_starts_[n] =
          last_start + std::distance(first, std::next(eol));
      first = std::next(eol);
    }
    // This will be equal to the total file size.
    line_starts_[line_count_] = last_start + std::distance(first, last);
  }

  std::size_t total_byte_size() const {
    return sizeof(size_t) * (line_count_ + 1);
  }

  boost::optional<std::size_t> find_start_of_line(size_t line_number) const {
    if (line_number > line_count_) {
      return boost::none;
    }
    if (line_number == 0) {
      return 0;
    } else {
      return line_starts_[line_number - 1];
    }
  }
};

#endif // __INDEX_HPP__
