//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2023 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/os.hpp"

#include "tenzir/concept/parseable/tenzir/ip.hpp"
#include "tenzir/concept/parseable/to.hpp"
#include "tenzir/config.hpp"
#include "tenzir/table_slice_builder.hpp"

#include <algorithm>

#if TENZIR_LINUX
#  include <pfs/procfs.hpp>
#elif TENZIR_MACOS
#  include <mach/mach_time.h>

#  include <libproc.h>
#endif

// The current state of the implementation is highly experimental. It's a POC to
// for some demos, in order to show that it's possible to get endpoint data if
// need be. The code is basically taking inspiration from Zeek Agent v2 at
// https://github.com/zeek/zeek-agent-v2/ and making it fit into Tenzir. None of
// this has been tested extensively.

namespace tenzir {

auto process_type() -> type {
  return type{
    "tenzir.process",
    record_type{
      {"name", string_type{}},
      {"pid", uint64_type{}},
      {"ppid", uint64_type{}},
      {"uid", uint64_type{}},
      {"gid", uint64_type{}},
      {"ruid", uint64_type{}},
      {"rgid", uint64_type{}},
      {"priority", string_type{}},
      {"startup", time_type{}},
      {"vsize", uint64_type{}},
      {"rsize", uint64_type{}},
      {"utime", duration_type{}},
      {"stime", duration_type{}},
    },
  };
}

auto socket_type() -> type {
  return type{
    "tenzir.socket",
    record_type{
      {"pid", uint64_type{}},
      {"process", string_type{}},
      {"protocol", uint64_type{}},
      {"local_addr", ip_type{}},
      {"local_port", uint64_type{}},
      {"remote_addr", ip_type{}},
      {"remote_port", uint64_type{}},
      {"state", string_type{}},
    },
  };
}

auto os::make() -> std::unique_ptr<os> {
#if TENZIR_LINUX
  return linux::make();
#elif TENZIR_MACOS
  return darwin::make();
#endif
  return nullptr;
}

auto os::processes() -> table_slice {
  auto builder = table_slice_builder{process_type()};
  for (const auto& proc : fetch_processes()) {
    auto okay = builder.add(proc.name, proc.pid, proc.ppid, proc.uid, proc.gid,
                            proc.ruid, proc.rgid, proc.priority, proc.startup);
    okay = builder.add(proc.vsize ? make_view(*proc.vsize) : data_view{});
    TENZIR_ASSERT(okay);
    okay = builder.add(proc.rsize ? make_view(*proc.rsize) : data_view{});
    TENZIR_ASSERT(okay);
    okay = builder.add(proc.utime ? make_view(*proc.utime) : data_view{});
    TENZIR_ASSERT(okay);
    okay = builder.add(proc.stime ? make_view(*proc.stime) : data_view{});
    TENZIR_ASSERT(okay);
  }
  return builder.finish();
}

auto os::sockets() -> table_slice {
  auto builder = table_slice_builder{socket_type()};
  for (const auto& proc : fetch_processes()) {
    auto pid = detail::narrow_cast<uint32_t>(proc.pid);
    for (const auto& socket : sockets_for(pid)) {
      auto okay = builder.add(uint64_t{proc.pid}, proc.name,
                              detail::narrow_cast<uint64_t>(socket.protocol),
                              socket.local_addr, uint64_t{socket.local_port},
                              socket.remote_addr, uint64_t{socket.remote_port},
                              not socket.state.empty() ? make_view(socket.state)
                                                       : data_view{});
      TENZIR_ASSERT(okay);
    }
  }
  return builder.finish();
}

#if TENZIR_LINUX

struct linux::state {
  uint64_t clock_tick{};
  pfs::procfs procfs{};
};

auto linux::make() -> std::unique_ptr<linux> {
  auto result = std::unique_ptr<linux>{new linux};
  result.clock_tick = sysconf(_SC_CLK_TCK);
  return result;
}

linux::linux() : state_{std::make_unique<state>()} {
}

linux::~linux() = default;

auto linux::fetch_processes() -> std::vector<process> {
  auto result = std::vector<process>{};
  try {
    auto tasks = state_->procfs.get_processes();
    result.reserve(tasks.size());
    for (const auto& task : tasks) {
      try {
        auto stat = task.get_stat();
        auto status = task.get_status();
        auto proc = process{
          .name = task.get_comm(),
          .pid = detail::narrow_cast<uint32_t>(task.id()),
          .ppid = detail::narrow_cast<uint32_t>(stat.ppid),
          .uid = detail::narrow_cast<uid_t>(status.uid.effective),
          .gid = detail::narrow_cast<gid_t>(status.gid.effective),
          .ruid = detail::narrow_cast<uid_t>(status.uid.real),
          .rgid = detail::narrow_cast<gid_t>(status.gid.real),
          .priority = std::to_string(stat.priority),
          .startup = {},
          .vsize = static_cast<uint64_t>(stat.vsize),
          .rsize = static_cast<uint64_t>(stat.rss * getpagesize()),
          .utime = std::chrono::seconds{stat.utime / state_->clock_tick},
          .stime = std::chrono::seconds{stat.stime / state_->clock_tick},
        };
        result.push_back(std::move(proc));
      } catch (std::system_error&) {
        TENZIR_DEBUG("ingoring exception for PID {}", task.id());
      } catch (std::runtime_error&) {
        TENZIR_DEBUG("ingoring exception for PID {}", task.id());
      }
    }
  } catch (const std::system_error&) {
    TENZIR_WARN("failed to read /proc filesystem (system error)");
  } catch (const std::runtime_error&) {
    TENZIR_WARN("failed to read /proc filesystem (runtime error)");
  }
  return result;
}

namespace {

auto to_string(pfs::net_socket::net_state state) -> std::string {
  switch (state) {
    default:
      break;
    case pfs::net_socket::net_state::close:
      return "CLOSED";
    case pfs::net_socket::net_state::close_wait:
      return "CLOSE_WAIT";
    case pfs::net_socket::net_state::closing:
      return "CLOSING";
    case pfs::net_socket::net_state::established:
      return "ESTABLISHED";
    case pfs::net_socket::net_state::fin_wait1:
      return "FIN_WAIT_1";
    case pfs::net_socket::net_state::fin_wait2:
      return "FIN_WAIT_2";
    case pfs::net_socket::net_state::last_ack:
      return "LAST_ACK";
    case pfs::net_socket::net_state::listen:
      return "LISTEN";
    case pfs::net_socket::net_state::syn_recv:
      return "SYN_RECEIVED";
    case pfs::net_socket::net_state::syn_sent:
      return "SYN_SENT";
    case pfs::net_socket::net_state::time_wait:
      return "TIME_WAIT";
  }
  return {};
}

auto to_socket(const pfs::net_socket& s, uint32_t pid, int protocol) -> socket {
  auto result = socket{};
  result.pid = pid;
  result.protocol = protocol;
  if (auto addr = to<ip>(s.local_ip.to_string()))
    result.local_addr = *addr;
  result.local_port = s.local_port;
  if (auto addr = to<ip>(s.remote_ip.to_string()))
    result.remote_addr = *addr;
  result.remote_port = s.remote_port;
  result.state = to_string(s.net_state);
}

} // namespace

auto linux::sockets_for(uint32_t pid) -> std::vector<socket> {
  auto result = std::vector<socket>{};
  auto add = [&](auto&& sockets, int proto) {
    for (const auto& socket : sockets)
      result.push_back(to_socket(socket, pid, proto));
  };
  auto net = state_->procfs.get_net(detail::narrow_cast<int>(pid));
  add(net.get_icmp(), IPPROTO_ICMP);
  add(net.get_icmp6(), IPPROTO_ICMP6);
  add(net.get_raw(), IPPROTO_RAW);
  add(net.get_raw6(), IPPROTO_RAW);
  add(net.get_tcp(), IPPROTO_TCP);
  add(net.get_tcp6(), IPPROTO_TCP);
  add(net.get_udp(), IPPROTO_UDP);
  add(net.get_udp6(), IPPROTO_UDP);
  add(net.get_udplite(), IPPROTO_UDPLITE);
  add(net.get_udplite6(), IPPROTO_UDPLITE);
  return result;
}

#elif TENZIR_MACOS

struct darwin::state {
  struct mach_timebase_info timebase {};
};

auto darwin::make() -> std::unique_ptr<darwin> {
  auto result = std::unique_ptr<darwin>{new darwin};
  if (mach_timebase_info(&result->state_->timebase) != KERN_SUCCESS) {
    TENZIR_ERROR("failed to get MACH timebase");
    return nullptr;
  }
  return result;
}

darwin::darwin() : state_{std::make_unique<state>()} {
}

darwin::~darwin() = default;

auto darwin::fetch_processes() -> std::vector<process> {
  auto num_procs = proc_listpids(PROC_ALL_PIDS, 0, nullptr, 0);
  std::vector<pid_t> pids;
  pids.resize(num_procs);
  num_procs = proc_listpids(PROC_ALL_PIDS, 0, pids.data(),
                            detail::narrow_cast<int>(pids.size()));
  if (num_procs <= 0) {
    TENZIR_ERROR("failed to get PIDs");
    return {};
  }
  auto result = std::vector<process>{};
  result.reserve(pids.size());
  for (auto pid : pids) {
    if (pid <= 0)
      continue;
    errno = 0;
    proc_bsdinfo proc{};
    auto n = proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &proc, sizeof(proc));
    if (n < detail::narrow_cast<int>(sizeof(proc)) || errno != 0) {
      if (errno == ESRCH) // process is gone
        continue;
      TENZIR_DEBUG("could not get process info for PID {}", pid);
      continue;
    }
    proc_taskinfo task{};
    n = proc_pidinfo(pid, PROC_PIDTASKINFO, 0, &task, sizeof(task));
    // Put it all together.
    auto runtime = std::chrono::seconds(proc.pbi_start_tvsec)
                   + std::chrono::microseconds(proc.pbi_start_tvusec);
    auto p = process{
      .name = std::string{static_cast<char*>(proc.pbi_name)},
      .pid = proc.pbi_pid,
      .ppid = proc.pbi_ppid,
      .uid = proc.pbi_uid,
      .gid = proc.pbi_gid,
      .ruid = proc.pbi_ruid,
      .rgid = proc.pbi_rgid,
      .priority = std::to_string(-proc.pbi_nice),
      .startup = time{runtime},
      .vsize = {},
      .rsize = {},
      .utime = {},
      .stime = {},
    };
    if (n < 0) {
      p.vsize = task.pti_virtual_size;
      p.rsize = task.pti_resident_size;
      auto timebase = state_->timebase;
      auto utime = task.pti_total_user * timebase.numer / timebase.denom;
      auto stime = task.pti_total_system * timebase.numer / timebase.denom;
      p.utime = std::chrono::nanoseconds(utime);
      p.stime = std::chrono::nanoseconds(stime);
    }
    result.push_back(std::move(p));
  }
  return result;
}

namespace {

auto socket_state_to_string(auto proto, auto state) -> std::string_view {
  switch (proto) {
    default:
      break;
    case 6: {
      switch (state) {
        default:
          break;
        case 0:
          return "CLOSED";
        case 1:
          return "LISTEN";
        case 2:
          return "SYN_SENT";
        case 3:
          return "SYN_RECEIVED";
        case 4:
          return "ESTABLISHED";
        case 5:
          return "CLOSE_WAIT";
        case 6:
          return "FIN_WAIT_1";
        case 7:
          return "CLOSING";
        case 8:
          return "LAST_ACK";
        case 9:
          return "FIN_WAIT_2";
        case 10:
          return "TIME_WAIT";
        case 11:
          return "RESERVED";
      }
    }
  }
  return "";
}

} // namespace

auto darwin::sockets_for(uint32_t pid) -> std::vector<socket> {
  auto p = detail::narrow_cast<pid_t>(pid);
  auto n = proc_pidinfo(p, PROC_PIDLISTFDS, 0, nullptr, 0);
  auto fds = std::vector<proc_fdinfo>{};
  fds.resize(n);
  n = proc_pidinfo(p, PROC_PIDLISTFDS, 0, fds.data(), n);
  if (n <= 0) {
    TENZIR_WARN("could not get file descriptors for process {}", p);
    return {};
  }
  auto result = std::vector<socket>{};
  for (auto& fd : fds) {
    if (fd.proc_fdtype != PROX_FDTYPE_SOCKET)
      continue;
    auto info = socket_fdinfo{};
    errno = 0;
    n = proc_pidfdinfo(p, fd.proc_fd, PROC_PIDFDSOCKETINFO, &info,
                       sizeof(socket_fdinfo));
    if (n < static_cast<int>(sizeof(socket_fdinfo)) or errno != 0)
      continue;
    // Only consider network connections.
    if (info.psi.soi_family != AF_INET and info.psi.soi_family != AF_INET6)
      continue;
    auto to_string = [](auto family, const auto& addr) -> std::string {
      auto buffer = std::array<char, INET6_ADDRSTRLEN>{};
      switch (family) {
        default:
          return {};
        case PF_INET:
          inet_ntop(family, &addr.ina_46.i46a_addr4, buffer.data(),
                    buffer.size());
          break;
        case PF_INET6:
          inet_ntop(family, &addr.ina_6, buffer.data(), buffer.size());
          break;
      };
      return std::string{buffer.data()};
    };
    // Populate socket.
    auto s = socket{};
    s.protocol = info.psi.soi_protocol;
    auto local_addr
      = to_string(info.psi.soi_family, info.psi.soi_proto.pri_in.insi_laddr);
    auto remote_addr
      = to_string(info.psi.soi_family, info.psi.soi_proto.pri_in.insi_faddr);
    if (auto addr = to<ip>(local_addr))
      s.local_addr = *addr;
    s.local_port = ntohs(info.psi.soi_proto.pri_in.insi_lport);
    if (auto addr = to<ip>(remote_addr))
      s.remote_addr = *addr;
    s.remote_port = ntohs(info.psi.soi_proto.pri_in.insi_fport);
    s.state = socket_state_to_string(info.psi.soi_protocol,
                                     info.psi.soi_proto.pri_tcp.tcpsi_state);
    result.push_back(std::move(s));
  }
  return result;
}

#endif

} // namespace tenzir
