//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2020 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include <caf/fwd.hpp>
#include <caf/response_promise.hpp>

#include <chrono>
#include <vector>

namespace vast::policy {

struct sequential;
struct parallel;

} // namespace vast::policy

namespace vast::system {

struct terminator_state {
  std::vector<caf::actor> remaining_actors;
  caf::response_promise promise;
  static inline const char* name = "terminator";
};

/// Performs a parallel shutdown of a list of actors.
/// @param self The terminator actor.
template <class Policy>
caf::behavior terminator(caf::stateful_actor<terminator_state>* self);

} // namespace vast::system
