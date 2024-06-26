//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2022 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/detail/partition_common.hpp"

#include "tenzir/evaluation_triple.hpp"
#include "tenzir/ids.hpp"

#include <unordered_map>

namespace tenzir::detail {

namespace {
bool needs_evaluation_without_indexer(const std::vector<evaluation_triple>& t) {
  return std::any_of(cbegin(t), cend(t), [](const auto& triple) {
    return !std::get<indexer_actor>(triple);
  });
}
} // namespace

ids get_ids_for_evaluation(
  const std::unordered_map<std::string, ids>& type_ids,
  const std::vector<evaluation_triple>& evaluation_triples) {
  if (needs_evaluation_without_indexer(evaluation_triples)) {
    auto all_ids = tenzir::ids{};
    for (const auto& [_, ids] : type_ids)
      all_ids |= ids;

    return all_ids;
  }

  return {};
}

} // namespace tenzir::detail
