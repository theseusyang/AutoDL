// Test funture and promis performance
// complie g++ -std=c++14 -I/home/tanguofu/workspace/seastar test_future.cpp
// 

#include "core/future.hh"
#include "core/reactor.hh"
#include "core/sleep.hh"
#include "core/app-template.hh"


#include <boost/intrusive/unordered_set.hpp>
#include <boost/intrusive/list.hpp>
#include <boost/intrusive_ptr.hpp>
#include <boost/lexical_cast.hpp>
#include <iomanip>
#include <sstream>
#include "core/app-template.hh"
#include "core/future-util.hh"
#include "core/timer-set.hh"
#include "core/shared_ptr.hh"
#include "core/stream.hh"
#include "core/memory.hh"
#include "core/units.hh"
#include "core/distributed.hh"
#include "core/vector-data-sink.hh"
#include "core/bitops.hh"
#include "core/slab.hh"
#include "core/align.hh"
#include "net/api.hh"
#include "net/packet-data-source.hh"
#include <unistd.h>

#define PLATFORM "seastar"
#define VERSION "v1.0"
#define VERSION_STRING PLATFORM " " VERSION

using namespace seastar;
using namespace net;

using clock_type = lowres_clock;

struct system_stats {
    uint32_t _curr_connections {};
    uint32_t _total_connections {};
    uint64_t _echo {};
    clock_type::time_point _start_time;
public:
    system_stats() {
        _start_time = clock_type::time_point::max();
    }
    system_stats(clock_type::time_point start_time)
        : _start_time(start_time) {
    }
    system_stats self() {
        return *this;
    }
    void operator+=(const system_stats& other) {
        _curr_connections += other._curr_connections;
        _total_connections += other._total_connections;
        _echo += other._echo;
        _start_time = std::min(_start_time, other._start_time);
    }
    future<> stop() { return make_ready_future<>(); }
};

class tcp_echo_server {
private:
    lw_shared_ptr<server_socket> _listener;
    system_stats _system_stats;
    uint16_t _port;
    struct connection {
        connected_socket _socket;
        socket_address _addr;
        input_stream<char> _in;
        output_stream<char> _out;
        system_stats& _system_stats;
        connection(connected_socket&& socket, socket_address addr, system_stats& stats)
            : _socket(std::move(socket))
            , _addr(addr)
            , _in(_socket.input())
            , _out(_socket.output())
            , _system_stats(stats)
        {
            _system_stats._curr_connections++;
            _system_stats._total_connections++;
        }
        ~connection() {
            _system_stats._curr_connections--;
        }

        std::vector<char> encode_reply_body(temporary_buffer<char>& payload) {
            std::vector<char> body;
            auto u = htonl(static_cast<uint32_t>(payload.size()));
            auto *s = reinterpret_cast<const char*>(&u);
            body.insert(body.end(), s, s + sizeof(u));
            body.insert(body.end(), payload.begin(), payload.end());
            return std::move(body);
        }

        uint32_t decode_reply_size(temporary_buffer<char>& data) {
            assert(data.size() == 4);
            auto p = reinterpret_cast<const uint8_t*>(data.get());
            uint32_t n = (static_cast<uint32_t>(p[0]) << 24)
                | (static_cast<uint32_t>(p[1]) << 16)
                | (static_cast<uint32_t>(p[2]) << 8)
                | (static_cast<uint32_t>(p[3]));
            return n;
        }
        future<> process() {
            return _in.read_exactly(4).then([this] (auto&& data) mutable {
                if (!data.empty()) {
                    ++_system_stats._echo;
                    auto payload_size = this->decode_reply_size(data);
                    return _in.read_exactly(payload_size).then([this] (auto&& data) {
                        if (!data.empty()) {
                            auto reply = this->encode_reply_body(data);
                            return _out.write(sstring { reply.data(), reply.size() });
                        }
                        return _in.close();
                    });
                }
                return _in.close();
            });
        }
    };
public:
    tcp_echo_server(uint16_t port = 11211) : _port(port) {}

    void start() {
        listen_options lo;
        lo.reuse_address = true;
        _listener = engine().listen(make_ipv4_address({_port}), lo);
        keep_doing([this] {
            return _listener->accept().then([this] (connected_socket fd, socket_address addr) mutable {
                auto conn = make_lw_shared<connection>(std::move(fd), addr, _system_stats);
                do_until([conn] { return conn->_in.eof(); }, [conn] {
                    return conn->process().then([conn] {
                        return conn->_out.flush();
                    });
                }).finally([conn] {
                    return conn->_out.close().finally([conn]{});
                });
            });
        }).or_terminate();
    }

    system_stats stats() {
        return _system_stats;
    }

    future<> stop() { return make_ready_future<>(); }
};

class stats_printer {
private:
    timer<> _timer;
    distributed<tcp_echo_server>& _shard_server;
    future<system_stats> stats() {
        return _shard_server.map_reduce(adder<system_stats>(), &tcp_echo_server::stats);
    }
    size_t _last {};
public:
    stats_printer(distributed<tcp_echo_server>& shard_server) : _shard_server(shard_server) {}

    void start() {
        _timer.set_callback([this] {
            stats().then([this] (auto stats) {
                std::cout << "current " << stats._curr_connections << " total " << stats._total_connections << " qps " << stats._echo - _last << "\n";
                _last = stats._echo;
            });
        });
        _timer.arm_periodic(std::chrono::seconds(1));
    }

    future<> stop() { return make_ready_future<>(); }
};


int main(int ac, char** av) {
    distributed<tcp_echo_server> shard_echo_server;
    stats_printer stats(shard_echo_server);

    namespace bpo = boost::program_options;
    app_template app;
    app.add_options()
        ("stats", "Print basic statistics periodically (every second)")
        ("port", bpo::value<uint16_t>()->default_value(10000), "Port listen on");

    return app.run_deprecated(ac, av, [&] {
        engine().at_exit([&] { return shard_echo_server.stop(); });

        auto&& config = app.configuration();
        uint16_t port = config["port"].as<uint16_t>();
        return shard_echo_server.start(port).then([&] {
            return shard_echo_server.invoke_on_all(&tcp_echo_server::start);
        }).then([&, port] {
            stats.start();
            std::cout << "TCP Echo-Server listen on: " << port << "\n";
        });
    });
}