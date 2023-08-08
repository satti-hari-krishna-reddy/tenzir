//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2023 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tenzir/fwd.hpp"

#include "tenzir/detail/debug_writer.hpp"
#include "tenzir/detail/default_formatter.hpp"

#include <fmt/format.h>

namespace tenzir {

/// Identifies a consecutive byte sequence within a source file.
///
/// If both offsets are zero, the location is unknown. Otherwise, the location
/// corresponds to the range `[begin, end)` in the main source file. In the
/// future, a `file` field might be added in order to support diagnostics from
/// multiple files simultaneously.
struct location {
  size_t begin{};
  size_t end{};

  /// The "unknown" location, where `begin` and `end` are 0.
  static const location unknown;

  /// Returns true if the location is known, and false otherwise.
  explicit operator bool() const {
    return *this != unknown;
  }

  auto
  subloc(size_t pos, size_t count = std::numeric_limits<size_t>::max()) const
    -> location {
    if (*this == unknown || pos > end) {
      return *this;
    }
    const auto first = begin + pos;
    const auto last = (count > end - first) ? end : (first + count);
    return {first, last};
  }

  auto operator<=>(const location&) const = default;

  friend auto inspect(auto& f, location& x) {
    if (auto dbg = as_debug_writer(f)) {
      return dbg->fmt_value("{}..{}", x.begin, x.end);
    }
    return f.object(x)
      .pretty_name("location")
      .fields(f.field("begin", x.begin), f.field("end", x.end));
  }
};

inline const location location::unknown = location{};

template <>
struct enable_default_formatter<location> : std::true_type {};

/// Provides a `T` together with a `location`.
template <class T>
struct located {
  T inner{};
  location source;

  located() = default;

  template <typename U = T>
    requires std::is_constructible_v<T, U&&>
  located(U&& inner, location source)
    : inner(std::forward<U>(inner)), source(source) {
  }

  template <typename U>
    requires std::is_constructible_v<T, const U&>
  explicit(!std::is_convertible_v<const U&, T>) located(const located<U>& other)
    : inner(other.inner), source(other.source) {
  }

  template <typename U>
    requires std::is_constructible_v<T, U&&>
  explicit(!std::is_convertible_v<U&&, T>) located(located<U>&& other)
    : inner(std::move(other.inner)), source(other.source) {
  }

  template <typename U>
    requires(std::is_constructible_v<T, const U&>
             && std::is_assignable_v<T&, const U&>)
  auto operator=(const located<U>& other) -> located& {
    inner = other.inner;
    source = other.source;
    return *this;
  }

  template <typename U>
    requires(std::is_constructible_v<T, U> && std::is_assignable_v<T&, U>)
  auto operator=(located<U&>& other) -> located& {
    inner = std::move(other.inner);
    source = other.source;
    return *this;
  }

  auto operator<=>(const located&) const = default;

  template <class Inspector>
  friend auto inspect(Inspector& f, located& x) {
    if (auto dbg = as_debug_writer(f)) {
      return dbg->apply(x.inner) && dbg->append(" @ {:?}", x.source);
    }
    return f.object(x).pretty_name("located").fields(
      f.field("inner", x.inner), f.field("source", x.source));
  }
};

template <class T>
located(T, location) -> located<T>;

template <class T>
struct enable_default_formatter<located<T>> : std::true_type {};

} // namespace tenzir
