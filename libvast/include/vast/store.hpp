//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2022 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "vast/fwd.hpp"

#include "vast/system/actors.hpp"

#include <caf/typed_event_based_actor.hpp>

namespace vast {

/// A base class for passive stores used by the store plugin.
class passive_store {
public:
  virtual ~passive_store() noexcept = default;

  /// Load the store contents from the given chunk.
  /// @param chunk The chunk pointing to the store's persisted data.
  /// @returns An error on failure.
  [[nodiscard]] virtual caf::error load(chunk_ptr chunk) = 0;

  /// Retrieve all of the store's slices.
  /// @returns The store's slices.
  [[nodiscard]] virtual const std::vector<table_slice>& slices() const = 0;
};

/// A base class for active stores used by the store plugin.
class active_store {
public:
  virtual ~active_store() noexcept = default;

  /// Add a set of slices to the store.
  /// @returns An error on failure.
  [[nodiscard]] virtual caf::error add(std::vector<table_slice> slices) = 0;

  /// Clear the store contents.
  /// @returns An error on failure.
  [[nodiscard]] virtual caf::error clear() = 0;

  /// Persist the store contents to a contiguous buffer.
  /// @returns A chunk containing the serialized store contents, or an error on
  /// failure.
  [[nodiscard]] virtual caf::expected<chunk_ptr> finish() = 0;

  /// Retrieve all of the store's slices.
  /// @returns The store's slices.
  [[nodiscard]] virtual const std::vector<table_slice>& slices() const = 0;
};

/// The state of the default passive store actor implementation.
struct default_passive_store_state {
  static constexpr auto name = "passive-store";

  system::store_actor::pointer self = {};
  system::filesystem_actor filesystem = {};
  system::accountant_actor accountant = {};
  std::unique_ptr<passive_store> store = {};
  std::filesystem::path path = {};
  std::string store_type = {};
};

/// Spawns a store actor for a passive store.
/// @param self A pointer to the hosting actor.
/// @param store The passive store to use.
/// @param filesystem A handle to the filesystem actor.
/// @param accountant A handle to the accountant actor.
/// @param path The path to load the store from.
/// @param store_type The unique store identifier of the used store plugin.
system::store_actor::behavior_type default_passive_store(
  system::store_actor::stateful_pointer<default_passive_store_state> self,
  std::unique_ptr<passive_store> store, system::filesystem_actor filesystem,
  system::accountant_actor accountant, std::filesystem::path path,
  std::string store_type);

/// The state of the default active store actor implementation.
struct default_active_store_state {
  static constexpr auto name = "active-store";

  system::store_builder_actor::pointer self = {};
  system::filesystem_actor filesystem = {};
  system::accountant_actor accountant = {};
  std::unique_ptr<active_store> store = {};
  std::filesystem::path path = {};
  std::string store_type = {};
};

/// Spawns a store builder actor for an active store.
/// @param self A pointer to the hosting actor.
/// @param store The active store to use.
/// @param filesystem A handle to the filesystem actor.
/// @param accountant A handle to the accountant actor.
/// @param path The path to persist the store at.
/// @param store_type The unique store identifier of the used store plugin.
system::store_builder_actor::behavior_type default_active_store(
  system::store_builder_actor::stateful_pointer<default_active_store_state> self,
  std::unique_ptr<active_store> store, system::filesystem_actor filesystem,
  system::accountant_actor accountant, std::filesystem::path path,
  std::string store_type);

} // namespace vast
