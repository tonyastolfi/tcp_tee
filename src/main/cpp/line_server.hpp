#ifndef __LINE_SERVER_HPP__
#define __LINE_SERVER_HPP__

#include <algorithm>
#include <cstddef>
#include <cstring>
#include <deque>
#include <iostream> // for clog
#include <istream>
#include <memory>
#include <sstream>
#include <string>

#include "boost/asio.hpp"

#include "indexed_file.hpp"

class LineServer {
private:
  class Connection {
  public:
    using socket_type = boost::asio::ip::tcp::socket;

  private:
    /// A conservative upper bound on the size of a valid request line.
    static const std::size_t MAX_LINE_SIZE = 100;

    boost::asio::io_service &io_;
    const IndexedFile &source_;
    socket_type socket_;
    boost::asio::streambuf input_buffer_;
    std::deque<boost::asio::const_buffer> output_buffers_;

  public:
    Connection(boost::asio::io_service &io, const IndexedFile &source)
        : io_(io), source_(source), socket_(io), input_buffer_(4096) {}

    socket_type &get_socket() { return socket_; }

    void notify_connected() { read_next_command(); }

  private:
    void read_next_command() {
      static const std::string ok = "OK\r\n";
      static const std::string err = "ERR\r\n";
      static const std::string crlf = "\r\n";

      // Try to read a line from the current buffer contents.
      if (!input_buffer_has_line()) {
        if (input_buffer_.size() > MAX_LINE_SIZE) {
          // Sorry dude, too long.
          std::unique_ptr<Connection> janitor(this);
          std::clog << "line too long: " << input_buffer_.size() << std::endl;
          return;
        }
        // Read more data.
        fill_buffer();
        return;
      }

      std::string line;
      {
        std::istream is(&input_buffer_);
        std::getline(is, line);
        if (!is.good()) {
          std::unique_ptr<Connection> janitor(this);
          std::clog << "failed to read line: " << line << std::endl;
          return;
        }
        input_buffer_.consume(line.length());
      }
      std::istringstream iss(line);
      std::string command;
      iss >> command;
      if (iss.fail()) {
        // No second chances.
        std::unique_ptr<Connection> janitor(this);
        std::clog << "parse error (command) in line: " << line << std::endl;
        return;
      }
      if ("GET" == command) {
        std::size_t line_number;
        iss >> line_number;
        if (iss.fail()) {
          std::unique_ptr<Connection> janitor(this);
          std::clog << "parse error (line number): " << line << std::endl;
          return;
        }
        if (line_number == 0) {
          output_buffers_.push_back(boost::asio::buffer(err));
        } else {
          const auto opt_line = source_.get_line(line_number - 1);
          if (!opt_line) {
            output_buffers_.push_back(boost::asio::buffer(err));
          } else {
            const std::size_t len =
                (!opt_line->empty() && *std::prev(opt_line->end()) == '\n')
                    ? (opt_line->size() - 1)
                    : opt_line->size();
            output_buffers_.push_back(boost::asio::buffer(ok));
            output_buffers_.push_back(
                boost::asio::buffer(opt_line->begin(), len));
            output_buffers_.push_back(boost::asio::buffer(crlf));
          }
        }
        flush_output();
      } else if ("QUIT" == command) {
        std::unique_ptr<Connection> janitor(this);
      } else if ("SHUTDOWN" == command) {
        socket_.get_io_service().stop();
      } else {
        std::unique_ptr<Connection> janitor(this);
        std::clog << "bad command: " << command << std::endl;
        return;
      }
    }

    bool input_buffer_has_line() const {
      for (const boost::asio::const_buffer &b : input_buffer_.data()) {
        if (std::memchr(boost::asio::buffer_cast<const char *>(b), '\n',
                        buffer_size(b))) {
          return true;
        }
      }
      return false;
    }

    void fill_buffer() {
      auto handler = [this](const boost::system::error_code &ec,
                            std::size_t bytes_read) {
        if (ec) {
          // All I/O errors are treated as fatal.
          std::unique_ptr<Connection> janitor(this);
          std::clog << "read error: " << ec.message() << std::endl;
          return;
        }
        input_buffer_.commit(bytes_read);
        read_next_command();
      };
      const auto mutable_buffers = input_buffer_.prepare(
          input_buffer_.max_size() - input_buffer_.size());
      socket_.async_read_some(mutable_buffers, handler);
    }

    void flush_output() {
      if (output_buffers_.empty()) {
        read_next_command();
        return;
      }

      auto handler = [this](const boost::system::error_code &ec,
                            std::size_t bytes_written) {
        if (ec) {
          std::unique_ptr<Connection> janitor(this);
          std::clog << "write error: " << ec.message() << std::endl;
          return;
        }
        while (bytes_written > 0 && !output_buffers_.empty()) {
          const std::size_t to_consume =
              std::min(bytes_written, buffer_size(output_buffers_.front()));
          output_buffers_.front() = output_buffers_.front() + to_consume;
          bytes_written -= to_consume;
          if (buffer_size(output_buffers_.front()) == 0) {
            output_buffers_.pop_front();
          }
        }
        if (output_buffers_.empty()) {
          read_next_command();
        } else {
          flush_output();
        }
      };

      // Write data!
      socket_.async_write_some(output_buffers_, handler);
    };
  };

  boost::asio::io_service &io_;
  boost::asio::ip::tcp::acceptor acceptor_;
  const IndexedFile &source_;
  std::unique_ptr<Connection> next_connection_;

public:
  LineServer(boost::asio::io_service &io, const IndexedFile &source)
      : io_(io), acceptor_(io_, boost::asio::ip::tcp::endpoint(
                                    boost::asio::ip::tcp::v4(), 10497)),
        source_(source) {}

  void start() {
    next_connection_.reset(new Connection(io_, source_));
    auto handler = [this](const boost::system::error_code &ec) {
      if (!ec) {
        auto *c = next_connection_.release();
        c->notify_connected();
      } else {
        std::clog << "accept error: " << ec.message() << std::endl;
      }
      start();
    };
    acceptor_.async_accept(next_connection_->get_socket(), handler);
  }
};

#endif // __LINE_SERVER_HPP__
