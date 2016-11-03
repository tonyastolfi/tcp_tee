#ifndef __INDEXED_FILE_HPP__
#define __INDEXED_FILE_HPP__

#include "boost/filesystem/operations.hpp"
#include "boost/range/iterator_range.hpp"

#include "line_index.hpp"

class IndexedFile {
private:
  const size_t size_;
  boost::interprocess::file_mapping mapping_;
  boost::interprocess::mapped_region region_;
  const char *const contents_;
  LineIndex index_;

public:
  IndexedFile(const char *const file, const char *index)
      : size_(boost::filesystem::file_size(file)),
        mapping_(file, boost::interprocess::read_only),
        region_(mapping_, boost::interprocess::read_only, 0, size_),
        contents_(reinterpret_cast<const char *>(region_.get_address())),
        index_(contents_, contents_ + size_, index) {}

  boost::optional<decltype(boost::make_iterator_range(contents_, contents_))>
  get_line(size_t line_number) const {
    const boost::optional<size_t> start =
        index_.find_start_of_line(line_number);
    const boost::optional<size_t> end =
        index_.find_start_of_line(line_number + 1);
    if (!start || !end) {
      return boost::none;
    }
    return boost::make_iterator_range(contents_ + *start, contents_ + *end);
  }
};

#endif // __INDEXED_FILE_HPP__
