#ifndef PROXY_SERVER_HPP
#define PROXY_SERVER_HPP

#include <boost/asio.hpp>
#include <boost/lexical_cast.hpp>

#include <atomic>
#include <cassert>
#include <fstream>
#include <iostream>

#define CLOG(expr) std::clog << expr << std::endl

class CircularBuffer {
public:
  explicit CircularBuffer(const int size_log_2) : storage_(1 << size_log_2) {}

  void commit(const size_t bytes) {
    assert(bytes <= writable_size());
    write_start_ += bytes;
  }

  void consume(const size_t bytes) {
    assert(bytes <= readable_size());
    read_start_ += bytes;
  }

  size_t capacity() const { return storage_.size(); }

  size_t readable_size() const { return write_start_ - read_start_; }

  size_t writable_size() { return capacity() - readable_size(); }

  std::vector<boost::asio::const_buffer> readable() const {
    assert(write_start_ - read_start_ <= storage_.size());
    return buffers<boost::asio::const_buffer>(read_start_ & mask_,
                                              readable_size(), storage_);
  }

  std::vector<boost::asio::mutable_buffer> writable() {
    assert(write_start_ - read_start_ <= storage_.size());
    return buffers<boost::asio::mutable_buffer>(write_start_ & mask_,
                                                writable_size(), storage_);
  }

  bool empty() const { return read_start_ == write_start_; }

  bool full() const { return write_start_ - read_start_ == storage_.size(); }

  friend std::ostream &operator<<(std::ostream &os, const CircularBuffer &b) {
    return os << "buffer(read=" << (b.read_start_ & b.mask_)
              << ", write=" << (b.write_start_ & b.mask_) << ")";
  }

private:
  template <typename T, typename U>
  static std::vector<T> buffers(size_t offset, size_t count, U &storage) {
    assert(count <= storage.size());
    if (offset + count <= storage.size()) {
      return {boost::asio::buffer(&storage[offset], count)};
    } else {
      return {
          boost::asio::buffer(&storage[offset], storage.size() - offset),
          boost::asio::buffer(&storage[0], offset + count - storage.size())};
    }
  }

  std::vector<char> storage_;
  size_t mask_ = storage_.size() - 1;
  size_t read_start_ = 0;
  size_t write_start_ = 0; // stays ahead of read start
};

class TransferOneWay {
public:
  using handler_type = std::function<void()>;

  int next_id() {
    static std::atomic<int> id{0};
    return ++id;
  }

  TransferOneWay(std::string name, boost::asio::ip::tcp::socket &source,
                 boost::asio::ip::tcp::socket &target,
                 boost::asio::io_service::strand &strand)
      : name_(name + boost::lexical_cast<std::string>(next_id())),
        source_(source), target_(target), strand_(strand), dump_(name_),
        flush_timer_(source_.get_io_service()) {}

  void start(handler_type handler) {
    {
      std::ostringstream oss;
      oss << name_ << "[" << source_.remote_endpoint() << " => "
          << source_.local_endpoint() << "]";
      read_label_ = oss.str();
    }
    {
      std::ostringstream oss;
      oss << name_ << "[" << target_.local_endpoint() << " => "
          << target_.remote_endpoint() << "]";
      write_label_ = oss.str();
    }
    handler_ = std::move(handler);
    start_flush_timer();
    start_io();
  }

private:
  void start_io() {
    if (!done_writing_ && !writing_) {
      if (!buffer_.empty()) {
        start_writing();
      } else if (done_reading_) {
        boost::system::error_code ec;
        target_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, ec);
        done_writing_ = true;
      }
    }
    if (!done_reading_ && !reading_ && !buffer_.full()) {
      start_reading();
    }
  }

  void start_flush_timer() {
    timer_running_ = true;
    flush_timer_.expires_from_now(boost::posix_time::seconds(5));
    flush_timer_.async_wait(
        strand_.wrap([this](const boost::system::error_code &ec) {
          timer_running_ = false;
          if (ec == boost::asio::error::operation_aborted || timer_cancelled_) {
            check_done();
            return;
          }

          dump_ << std::flush;
          start_flush_timer();
        }));
  }

  void start_reading() {
    assert(!reading_);
    assert(!done_reading_);
    reading_ = true;
    CLOG(read_label_ << ": reading...");
    source_.async_read_some(
        buffer_.writable(),
        strand_.wrap([this](const boost::system::error_code &ec, size_t count) {
          reading_ = false;
          if (ec) {
            CLOG(read_label_ << ": read error '" << ec.message() << "'");
            done_reading_ = true;
            start_io();
            check_done();
            return;
          }
          CLOG(read_label_ << ": read " << count << " bytes");
          buffer_.commit(count);
          assert(count > 0);
          assert(!buffer_.empty());
          start_io();
        }));
  }

  void start_writing() {
    assert(!writing_);
    assert(!done_writing_);
    writing_ = true;
    CLOG(write_label_ << ": writing... " << buffer_);
    assert(!buffer_.empty());
    assert(buffer_.readable_size() > 0);
    target_.async_write_some(
        buffer_.readable(),
        strand_.wrap([this](const boost::system::error_code &ec, size_t count) {
          writing_ = false;
          if (ec) {
            CLOG(write_label_ << ": write error '" << ec.message() << "'");
            done_writing_ = true;
            boost::system::error_code e;
            source_.shutdown(boost::asio::ip::tcp::socket::shutdown_receive, e);
            target_.shutdown(boost::asio::ip::tcp::socket::shutdown_send, e);
            check_done();
            return;
          }
          CLOG(write_label_ << ": wrote " << count << " bytes");
          {
            auto data = buffer_.readable();
            size_t remaining = count;
            for (const auto &cb : data) {
              const size_t len =
                  std::min(remaining, boost::asio::buffer_size(cb));
              try {
                dump_.write(boost::asio::buffer_cast<const char *>(cb), len);
              } catch (std::exception &e) {
                CLOG(write_label_ << ": failed to dump data: " << e.what());
                done_writing_ = true;
                check_done();
                return;
              }
              remaining -= len;
            }
          }
          buffer_.consume(count);
          assert(count > 0);
          assert(!buffer_.full());
          start_io();
        }));
  }

  void check_done() {
    if (!reading_ && !writing_ && done_reading_) {
      if (timer_running_) {
        timer_cancelled_ = true;
        flush_timer_.cancel();
      } else {
        CLOG("done: " << read_label_ << ", " << write_label_);
        handler_();
      }
    }
  }

  std::string name_;
  std::string read_label_;
  std::string write_label_;
  CircularBuffer buffer_{9};
  boost::asio::ip::tcp::socket &source_;
  boost::asio::ip::tcp::socket &target_;
  boost::asio::io_service::strand &strand_;
  bool reading_ = false;
  bool writing_ = false;
  bool done_reading_ = false;
  bool done_writing_ = false;
  handler_type handler_;
  std::ofstream dump_;
  boost::asio::deadline_timer flush_timer_;
  bool timer_running_ = false;
  bool timer_cancelled_ = false;
};

class ProxyConnection {
public:
  ProxyConnection(std::unique_ptr<boost::asio::ip::tcp::socket> s,
                  const boost::asio::ip::tcp::endpoint &target_endpoint)
      : client_(std::move(s)), target_(client_->get_io_service()),
        strand_(client_->get_io_service()), target_endpoint_(target_endpoint) {}

  void start() {
    auto handler = [this](const boost::system::error_code &ec) {
      if (ec) {
        CLOG("connect to target error: " << ec.message());
        delete this;
        return;
      }
      CLOG("connected to " << target_endpoint_ << "["
                           << target_.local_endpoint() << "]");
      client_to_target_.start([this]() { done(); });
      target_to_client_.start([this]() { done(); });
    };
    target_.async_connect(target_endpoint_, strand_.wrap(handler));
  }

private:
  void done() {
    if (--activity_ == 0) {
      CLOG("connection closed.");
      delete this;
    }
  }

  std::unique_ptr<boost::asio::ip::tcp::socket> client_;
  boost::asio::ip::tcp::socket target_;
  boost::asio::io_service::strand strand_;
  boost::asio::ip::tcp::endpoint target_endpoint_;
  int activity_ = 2;
  TransferOneWay client_to_target_{"client-to-target", *client_, target_,
                                   strand_};
  TransferOneWay target_to_client_{"target-to-client", target_, *client_,
                                   strand_};
};

class ProxyServer {
public:
  ProxyServer(boost::asio::io_service &io,
              const boost::asio::ip::tcp::endpoint &bind_to,
              const boost::asio::ip::tcp::endpoint &target)
      : io_(io), bind_to_(bind_to), target_(target), acceptor_(io, bind_to_) {}

  void start() {
    next_connection_.reset(new boost::asio::ip::tcp::socket(io_));
    auto handler = [this](const boost::system::error_code &ec) {
      if (ec) {
        CLOG("accept error: " << ec.message());
      } else {
        CLOG("accepted connection[" << next_connection_->remote_endpoint()
                                    << "]");
        (new ProxyConnection(std::move(next_connection_), target_))->start();
      }
      start();
    };
    CLOG("waiting for connection on " << bind_to_);
    acceptor_.async_accept(*next_connection_, handler);
  }

private:
  boost::asio::io_service &io_;
  const boost::asio::ip::tcp::endpoint bind_to_;
  const boost::asio::ip::tcp::endpoint target_;
  boost::asio::ip::tcp::acceptor acceptor_;
  std::unique_ptr<boost::asio::ip::tcp::socket> next_connection_;
};

#endif // PROXY_SERVER_HPP
