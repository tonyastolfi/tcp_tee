#include <iostream>
#include <string>

#include <boost/thread/thread.hpp>

#include "proxy_server.hpp"

int main(int argc, char **argv) {
  boost::asio::io_service io;
  boost::asio::ip::tcp::resolver resolver(io);
  boost::asio::ip::tcp::endpoint ep[2];
  for (int i = 0; i < 2; ++i) {
    boost::asio::ip::tcp::resolver::query q(argv[1 + i * 2], argv[2 + i * 2]);
    auto it = resolver.resolve(q);
    decltype(it) end;
    if (it == end) {
      std::cerr << "failed to resolve bind endpoint" << std::endl;
      return 1;
    }
    ep[i] = *it;
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
