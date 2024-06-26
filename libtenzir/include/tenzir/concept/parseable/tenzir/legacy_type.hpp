//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2016 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#pragma once

#include "tenzir/concept/parseable/core/operators.hpp"
#include "tenzir/concept/parseable/core/parser.hpp"
#include "tenzir/concept/parseable/string/any.hpp"
#include "tenzir/concept/parseable/string/char_class.hpp"
#include "tenzir/concept/parseable/string/literal.hpp"
#include "tenzir/legacy_type.hpp"
#include "tenzir/type.hpp"

namespace tenzir {

/// Parses a type into an intermediate representation.
/// References to user defined types are mapped to `legacy_none_type` and
/// need to be resolved later.
struct legacy_type_parser : parser_base<legacy_type_parser> {
  using attribute = legacy_type;

  // Comments until the end of line.
  static constexpr auto comment
    = ignore(parsers::lit{"//"} >> *(parsers::any - '\n'));

  // Skips all irrelevant tokens.
  static constexpr auto skp = ignore(*(parsers::space | comment));

  template <class Iterator, class Attribute>
  bool parse(Iterator& f, const Iterator& l, Attribute& a) const;
};

template <>
struct parser_registry<legacy_type> {
  using type = legacy_type_parser;
};

namespace parsers {

inline const auto legacy_type = legacy_type_parser{};

} // namespace parsers
} // namespace tenzir
