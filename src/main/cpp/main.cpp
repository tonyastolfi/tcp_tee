#include <iostream>
#include <string>

#include <boost/thread/thread.hpp>

#include "proxy_server.hpp"

int main(int argc, char **argv) {
  boost::asio::io_service io;
  boost::asio::ip::tcp::resolver resolver(io);
  boost::asio::ip::tcp::endpoint ep[2];
  for (int i = 0; i < 2; ++i) {
    std::string host_name = argv[1 + i * 2];
    std::string service_port = argv[2 + i * 2];
    boost::asio::ip::tcp::resolver::query q(host_name, service_port);
    try {
      auto it = resolver.resolve(q);
      decltype(it) end;
      if (it == end) {
        std::cerr << "failed to resolve bind endpoint" << std::endl;
        return 1;
      }
      ep[i] = *it;
    } catch (...) {
      std::cerr << "resolving " << host_name << ":" << service_port
                << std::endl;
      throw;
    }
  }

  ProxyServer server(io, ep[0], ep[1]);
  boost::thread_group pool;
  {
    server.start();
    for (int n = 0; n < 1; ++n) {
      pool.add_thread(new boost::thread([&io] { io.run(); }));
    }
  }
  pool.join_all();
  return 0;
}
