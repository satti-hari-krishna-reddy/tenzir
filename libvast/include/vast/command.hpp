//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2018 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vast/fwd.hpp"

#include "vast/detail/string.hpp"

#include <caf/config_option_set.hpp>
#include <caf/error.hpp>
#include <caf/fwd.hpp>
#include <fmt/format.h>

#include <functional>
#include <iosfwd>
#include <map>
#include <memory>
#include <string>
#include <string_view>

namespace vast {

/// A named command with optional children.
class command {
public:
  // -- member types -----------------------------------------------------------

  /// Iterates over CLI arguments.
  using argument_iterator = std::vector<std::string>::const_iterator;

  /// Stores child commands.
  using children_list = std::vector<std::unique_ptr<command>>;

  /// Delegates to the command implementation logic.
  using fun
    = std::function<caf::message(const invocation&, caf::actor_system&)>;

  /// Central store for mapping fully-qualified command name to callback
  using factory = std::map<std::string, fun>;

  /// Builds config options for the same category.
  class opts_builder {
  public:
    explicit opts_builder(std::string_view category) : category_(category) {
      // nop
    }

    opts_builder(std::string_view category, caf::config_option_set xs)
      : category_(category), xs_(std::move(xs)) {
      // nop
    }

    /// Adds a config option to the category.
    template <class T>
    opts_builder&& add(std::string_view name, std::string_view description) && {
      xs_.add(caf::make_config_option<T>(category_, name, description));
      return std::move(*this);
    }

    /// Extracts the options from this builder.
    caf::config_option_set finish() {
      return std::move(xs_);
    }

  private:
    /// Category for all options generated by this adder.
    std::string_view category_;

    /// Our set-under-construction.
    caf::config_option_set xs_;
  };

  // -- member variables -------------------------------------------------------

  /// A pointer to the parent node (or nullptr iff this is the root node).
  command* parent;

  /// The name of the command.
  std::string_view name;

  /// A short phrase that describes the command, e.g., "prints the help text".
  std::string_view description;

  /// Detailed usage instructions written in Markdown.
  std::string_view documentation;

  /// The options of the command.
  caf::config_option_set options;

  /// The list of sub-commands.
  children_list children;

  /// Flag that indicates whether the command shows up in the help text.
  bool visible;

  // -- constructors -----------------------------------------------------------

  /// Construct a new command
  command(std::string_view name, std::string_view description,
          std::string_view documentation, caf::config_option_set opts,
          bool visible = true);

  /// Construct a new command
  command(std::string_view name, std::string_view description,
          std::string_view documentation, opts_builder opts,
          bool visible = true);

  command(command&&) = delete;
  command(const command&) = delete;
  command& operator=(command&&) = delete;
  command& operator=(const command&) = delete;

  // -- utility functions ------------------------------------------------------

  /// Returns the full name of `cmd`, i.e., its own name prepended by all parent
  /// names.
  [[nodiscard]] std::string full_name() const;

  // -- factory functions ------------------------------------------------------

  /// Creates a config option set pre-initialized with a help option.
  static caf::config_option_set opts();

  /// Creates a config option set pre-initialized with a help option.
  static opts_builder opts(std::string_view category);

  /// Adds a new subcommand.
  /// @returns a pointer to the new subcommand.
  command* add_subcommand(std::unique_ptr<command> cmd) {
    auto result = children.emplace_back(std::move(cmd)).get();
    result->parent = this;
    return result;
  }

  /// Adds a new subcommand.
  /// @returns a pointer to the new subcommand.
  template <typename... Ts>
  command* add_subcommand(std::string_view name, std::string_view description,
                          Ts&&... args) {
    auto result = children
                    .emplace_back(std::make_unique<command>(
                      name, description, std::forward<Ts>(args)...))
                    .get();
    result->parent = this;
    return result;
  }
};

/// Wraps invocation of a single command for separating the parsing of
/// program argument from running the command.
struct invocation {
  // -- member variables -----------------------------------------------------

  /// Stores user-defined program options.
  caf::settings options;

  /// Holds the fully-qualified name of the scheduled command.
  std::string full_name;

  /// Holds the CLI arguments.
  std::vector<std::string> arguments;

  // -- utility methods ------------------------------------------------------

  /// Holds the name of the scheduled command.
  [[nodiscard]] std::string_view name() const {
    std::string_view result = full_name;
    result.remove_prefix(std::min(result.find_last_of(' ') + 1, result.size()));
    return result;
  }

  template <class Inspector>
  friend auto inspect(Inspector& f, invocation& x) {
    return f(caf::meta::type_name("invocation"), x.full_name, x.arguments,
             x.options);
  }

  // -- mutators -------------------------------------------------------------

  /// Sets the members `full_name`, and `arguments`.
  void assign(const command* cmd, command::argument_iterator first,
              command::argument_iterator last) {
    full_name = cmd->full_name();
    arguments = {first, last};
  }
};

/// Parses all program arguments without running the command.
/// @returns an error for malformed input, `none` otherwise.
/// @relates command
caf::expected<invocation>
parse(const command& root, command::argument_iterator first,
      command::argument_iterator last);

/// Runs the command and blocks until execution completes.
/// @returns a type-erased result or a wrapped `caf::error`.
/// @relates command
caf::expected<caf::message>
run(const invocation& invocation, caf::actor_system& sys,
    const command::factory& fact);

/// Traverses the command hierarchy until finding the root.
/// @returns the root command.
const command& root(const command& cmd);

/// Gets a subcommand from its full name.
/// @param cmd The parent to search for *position.
/// @param position The next subcommand to resolve.
/// @param end The position after the last subcommand name.
/// @returns A pointer to the corresponding command on success, or nullptr
///          on error.
/// @relates command
const command* resolve(const command& cmd,
                       std::vector<std::string_view>::iterator position,
                       std::vector<std::string_view>::iterator end);

/// Gets a subcommand from its full name.
/// @param cmd The parent to search for *position.
/// @param name A whitespace separated sequence of subcommands.
/// @returns A pointer to the corresponding command on success, or nullptr
///          on error.
/// @relates command
const command* resolve(const command& cmd, std::string_view name);

/// Prints the helptext for `cmd` to `out`.
void helptext(const command& cmd, std::ostream& out);

/// Returns the helptext for `cmd`.
std::string helptext(const command& cmd);

/// Applies `fun` to `cmd` and each of its children, recursively.
template <class F>
void for_each(const command& cmd, F fun) {
  fun(cmd);
  for (auto& ptr : cmd.children)
    for_each(*ptr, fun);
}

} // namespace vast

namespace fmt {

template <>
struct formatter<vast::invocation> : formatter<std::string> {
  template <typename FormatContext>
  auto format(const vast::invocation& item, FormatContext& ctx) {
    return formatter<std::string>::format(caf::deep_to_string(item), ctx);
  }
};

} // namespace fmt