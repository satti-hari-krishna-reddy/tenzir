//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2023 The VAST Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "vast/argument_parser.hpp"
#include "vast/arrow_table_slice.hpp"
#include "vast/cast.hpp"
#include "vast/concept/parseable/string/any.hpp"
#include "vast/concept/parseable/vast/option_set.hpp"
#include "vast/concept/parseable/vast/pipeline.hpp"
#include "vast/concept/printable/to_string.hpp"
#include "vast/concept/printable/vast/json.hpp"
#include "vast/data.hpp"
#include "vast/detail/string.hpp"
#include "vast/detail/string_literal.hpp"
#include "vast/detail/to_xsv_sep.hpp"
#include "vast/detail/zeekify.hpp"
#include "vast/detail/zip_iterator.hpp"
#include "vast/generator.hpp"
#include "vast/plugin.hpp"
#include "vast/table_slice_builder.hpp"
#include "vast/to_lines.hpp"
#include "vast/type.hpp"
#include "vast/view.hpp"

#include <arrow/record_batch.h>
#include <caf/error.hpp>
#include <caf/expected.hpp>
#include <caf/none.hpp>
#include <caf/typed_event_based_actor.hpp>
#include <fmt/core.h>
#include <fmt/format.h>

#include <algorithm>
#include <cctype>
#include <iterator>
#include <string>
#include <string_view>

namespace vast::plugins::zeek_tsv {

namespace {

template <concrete_type Type>
struct zeek_parser {
  auto operator()(const Type&, char, const std::string&) const
    -> rule<std::string_view::const_iterator, type_to_data_t<Type>> {
    die("unexpected type");
  }
};

template <>
struct zeek_parser<bool_type> {
  auto operator()(const bool_type&, char, const std::string&) const {
    return parsers::tf;
  }
};

template <>
struct zeek_parser<int64_type> {
  auto operator()(const int64_type&, char, const std::string&) const {
    return parsers::i64;
  }
};

template <>
struct zeek_parser<uint64_type> {
  auto operator()(const uint64_type&, char, const std::string&) const {
    return parsers::u64;
  }
};

template <>
struct zeek_parser<double_type> {
  auto operator()(const double_type&, char, const std::string&) const {
    return parsers::real.then([](double x) {
      return x;
    });
  }
};

template <>
struct zeek_parser<duration_type> {
  auto operator()(const duration_type&, char, const std::string&) const {
    return parsers::real.then([](double x) {
      return std::chrono::duration_cast<duration>(double_seconds(x));
    });
  }
};

template <>
struct zeek_parser<time_type> {
  auto operator()(const time_type&, char, const std::string&) const {
    return parsers::real.then([](double x) {
      return time{} + std::chrono::duration_cast<duration>(double_seconds(x));
    });
  }
};

template <>
struct zeek_parser<string_type> {
  auto operator()(const string_type&, char separator,
                  const std::string& set_separator) const
    -> rule<std::string_view::const_iterator, std::string> {
    if (set_separator.empty()) {
      return (+(parsers::any - separator)).then([](std::string x) {
        return detail::byte_unescape(x);
      });
    }
    return (+(parsers::any - separator - set_separator)).then([](std::string x) {
      return detail::byte_unescape(x);
    });
  }
};

template <>
struct zeek_parser<ip_type> {
  auto operator()(const ip_type&, char, const std::string&) const {
    return parsers::ip;
  }
};

template <>
struct zeek_parser<subnet_type> {
  auto operator()(const subnet_type&, char, const std::string&) const {
    return parsers::ip;
  }
};

template <>
struct zeek_parser<list_type> {
  auto operator()(const list_type& lt, char separator,
                  const std::string& set_separator) const {
    auto f
      = [&]<concrete_type Type>(
          const Type& type) -> rule<std::string_view::const_iterator, list> {
      return (zeek_parser<Type>{}(type, separator, set_separator)
                .then([](type_to_data_t<Type> value) {
                  return data{value};
                })
              % set_separator);
    };
    return caf::visit(f, lt.value_type());
  }
};

// Creates a VAST type from an ASCII Zeek type in a log header.
auto parse_type(std::string_view zeek_type) -> caf::expected<type> {
  type t;
  if (zeek_type == "enum" or zeek_type == "string" or zeek_type == "file"
      or zeek_type == "pattern")
    t = type{string_type{}};
  else if (zeek_type == "bool")
    t = type{bool_type{}};
  else if (zeek_type == "int")
    t = type{int64_type{}};
  else if (zeek_type == "count")
    t = type{uint64_type{}};
  else if (zeek_type == "double")
    t = type{double_type{}};
  else if (zeek_type == "time")
    t = type{time_type{}};
  else if (zeek_type == "interval")
    t = type{duration_type{}};
  else if (zeek_type == "addr")
    t = type{ip_type{}};
  else if (zeek_type == "subnet")
    t = type{subnet_type{}};
  else if (zeek_type == "port")
    // FIXME: once we ship with builtin type aliases, we should reference the
    // port alias type here. Until then, we create the alias manually.
    // See also:
    // - src/format/pcap.cpp
    t = type{"port", uint64_type{}};
  if (!t
      && (zeek_type.starts_with("vector") or zeek_type.starts_with("set")
          or zeek_type.starts_with("table"))) {
    // Zeek's logging framwork cannot log nested vectors/sets/tables, so we can
    // safely assume that we're dealing with a basic type inside the brackets.
    // If this will ever change, we'll have to enhance this simple parser.
    auto open = zeek_type.find('[');
    auto close = zeek_type.rfind(']');
    if (open == std::string::npos or close == std::string::npos)
      return caf::make_error(ec::format_error, "missing container brackets:",
                             std::string{zeek_type});
    auto elem = parse_type(zeek_type.substr(open + 1, close - open - 1));
    if (!elem)
      return elem.error();
    // Zeek sometimes logs sets as tables, e.g., represents set[string] as
    // table[string]. In VAST, they are all lists.
    t = type{list_type{*elem}};
  }
  if (!t)
    return caf::make_error(ec::format_error,
                           "failed to parse type: ", std::string{zeek_type});
  return t;
}

struct zeek_metadata {
  using iterator_type = std::string_view::const_iterator;

  auto is_unset(std::string_view field) -> bool {
    return std::equal(unset_field.begin(), unset_field.end(), field.begin(),
                      field.end());
  }

  auto is_empty(std::string_view field) -> bool {
    return std::equal(empty_field.begin(), empty_field.end(), field.begin(),
                      field.end());
  }

  auto make_parser(const auto& type, const auto& set_sep) {
    return make_zeek_parser<iterator_type>(type, set_sep);
  };

  std::string sep{};
  int sep_char{};
  std::string set_sep{};
  std::string empty_field{};
  std::string unset_field{};
  std::string path{};
  std::string fields_str{};
  std::string types_str{};
  std::vector<std::string_view> fields{};
  std::vector<std::string_view> types{};
  std::string name{};
  std::vector<struct record_type::field> record_fields{};
  type output_slice_schema{};
  type temp_slice_schema{};
  std::vector<rule<iterator_type, data>> parsers{};
  std::vector<std::string> parsed_options{};
  std::string header{};
  std::array<std::string_view, 7> prefix_options{
    "#set_separator", "#empty_field", "#unset_field", "#path",
    "#open",          "#fields",      "#types",
  };
};

struct zeek_printer {
  zeek_printer(char set_sep, std::string_view empty = "",
               std::string_view unset = "", bool disable_timestamp_tags = false)
    : set_sep{set_sep},
      empty_field{empty},
      unset_field{unset},
      disable_timestamp_tags{disable_timestamp_tags} {
  }

  auto to_zeek_string(const type& t) const -> std::string {
    auto f = detail::overload{
      [](const bool_type&) -> std::string {
        return "bool";
      },
      [](const int64_type&) -> std::string {
        return "int";
      },
      [&](const uint64_type&) -> std::string {
        return t.name() == "port" ? "port" : "count";
      },
      [](const double_type&) -> std::string {
        return "double";
      },
      [](const duration_type&) -> std::string {
        return "interval";
      },
      [](const time_type&) -> std::string {
        return "time";
      },
      [](const string_type&) -> std::string {
        return "string";
      },
      [](const ip_type&) -> std::string {
        return "addr";
      },
      [](const subnet_type&) -> std::string {
        return "subnet";
      },
      [](const enumeration_type&) -> std::string {
        return "enumeration";
      },
      [this](const list_type& lt) -> std::string {
        return fmt::format("vector[{}]", to_zeek_string(lt.value_type()));
      },
      [](const map_type&) -> std::string {
        return "map";
      },
      [](const record_type&) -> std::string {
        return "record";
      },
    };
    return t ? caf::visit(f, t) : "none";
  }

  auto generate_timestamp() const -> std::string {
    auto now = std::chrono::system_clock::now();
    return fmt::format(timestamp_format, now);
  }

  template <typename It>
  auto print_header(It& out, const type& t) const noexcept -> bool {
    auto header = fmt::format("#separator \\x{0:02x}\n"
                              "#set_separator{0}{1}\n"
                              "#empty_field{0}{2}\n"
                              "#unset_field{0}{3}\n"
                              "#path{0}{4}",
                              sep, set_sep, empty_field, unset_field, t.name());
    if (not disable_timestamp_tags)
      header.append(fmt::format("\n#open{}{}", sep, generate_timestamp()));
    header.append("\n#fields");
    auto r = caf::get<record_type>(t);
    for (const auto& [_, offset] : r.leaves())
      header.append(fmt::format("{}{}", sep, to_string(r.key(offset))));
    header.append("\n#types");
    for (const auto& [field, _] : r.leaves())
      header.append(fmt::format("{}{}", sep, to_zeek_string(field.type)));
    out = std::copy(header.begin(), header.end(), out);
    return true;
  }

  template <typename It>
  auto print_values(It& out, const view<record>& x) const noexcept -> bool {
    auto first = true;
    for (const auto& [_, v] : x) {
      if (not first) {
        ++out = sep;
      } else {
        first = false;
      }
      caf::visit(visitor{out, *this}, v);
    }
    return true;
  }

  template <typename It>
  auto print_closing_line(It& out) const noexcept -> void {
    if (not disable_timestamp_tags) {
      out = fmt::format_to(out, "#close{}{}\n", sep, generate_timestamp());
    }
  }

  template <class Iterator>
  struct visitor {
    visitor(Iterator& out, const zeek_printer& printer)
      : out{out}, printer{printer} {
    }

    auto operator()(caf::none_t) noexcept -> bool {
      out = std::copy(printer.unset_field.begin(), printer.unset_field.end(),
                      out);
      return true;
    }

    auto operator()(auto x) noexcept -> bool {
      make_printer<decltype(x)> p;
      return p.print(out, x);
    }

    auto operator()(view<bool> x) noexcept -> bool {
      return printers::any.print(out, x ? 'T' : 'F');
    }

    auto operator()(view<pattern>) noexcept -> bool {
      die("unreachable");
    }

    auto operator()(view<map>) noexcept -> bool {
      die("unreachable");
    }

    auto operator()(view<std::string> x) noexcept -> bool {
      if (x.empty()) {
        out = std::copy(printer.unset_field.begin(), printer.unset_field.end(),
                        out);
        return true;
      }
      for (auto c : x)
        if (std::iscntrl(c) or c == printer.sep or c == printer.set_sep) {
          auto hex = detail::byte_to_hex(c);
          *out++ = '\\';
          *out++ = 'x';
          *out++ = hex.first;
          *out++ = hex.second;
        } else {
          *out++ = c;
        }
      return true;
    }

    auto operator()(const view<list>& x) noexcept -> bool {
      if (x.empty()) {
        out = std::copy(printer.empty_field.begin(), printer.empty_field.end(),
                        out);
        return true;
      }
      auto first = true;
      for (const auto& v : x) {
        if (not first) {
          ++out = printer.set_sep;
        } else {
          first = false;
        }
        caf::visit(*this, v);
      }
      return true;
    }

    auto operator()(const view<record>& x) noexcept -> bool {
      // TODO: This won't be needed when flatten() for table_slices is in the
      // codebase.
      VAST_WARN("printing records as zeek-tsv data is currently a work in "
                "progress; printing null instead");
      auto first = true;
      for (const auto& v : x) {
        if (not first) {
          ++out = printer.sep;
        } else {
          first = false;
        }
        (*this)(caf::none);
      }
      return true;
    }

    Iterator& out;
    const zeek_printer& printer;
  };

  static constexpr auto timestamp_format{"{:%Y-%m-%d-%H-%M-%S}"};
  char sep{'\t'};
  char set_sep{','};
  std::string empty_field{};
  std::string unset_field{};
  bool disable_timestamp_tags{false};
};

struct zeek_document {
  /// Optional metadata.
  char separator = '\t';
  std::string set_separator = ",";
  std::string empty_field = "(empty)";
  std::string unset_field = "-";

  // Required metadata.
  std::string path = {};
  std::vector<std::string> fields = {};
  std::vector<std::string> types = {};

  /// A builder generated from the above metadata.
  std::optional<table_slice_builder> builder = {};
  std::vector<rule<std::string_view::const_iterator, bool>> parsers = {};
  type target_schema = {};
};

auto parser_impl(generator<std::optional<std::string_view>> lines,
                 operator_control_plane& ctrl) -> generator<table_slice> {
  auto document = zeek_document{};
  auto last_finish = std::chrono::steady_clock::now();
  auto line_nr = size_t{0};
  for (auto&& line : lines) {
    const auto now = std::chrono::steady_clock::now();
    // Yield at chunk boundaries.
    if (document.builder
        and (document.builder->rows() >= defaults::import::table_slice_size
             or last_finish + std::chrono::seconds{1} < now)) {
      last_finish = now;
      co_yield cast(document.builder->finish(), document.target_schema);
    }
    if (not line) {
      if (last_finish != now) {
        co_yield {};
      }
      continue;
    }
    // We keep track of the line number for better diagnostics.
    ++line_nr;
    // Skip empty lines unconditionally.
    if (line->empty()) {
      continue;
    }
    // Parse document lines.
    if (line->starts_with('#')) {
      auto header = line->substr(1);
      const auto separator = ignore(parsers::chr{document.separator});
      const auto unescaped_str
        = (+(parsers::any - separator)).then([](std::string separator) {
            return detail::byte_unescape(separator);
          });
      // Handle the closing header.
      const auto close_parser
        = ("close" >> separator >> unescaped_str).then([&](std::string close) {
            // This contains a timestamp of the format
            // YYYY-DD-MM-hh-mm-ss that we currently
            // ignore.
            (void)close;
          });
      if (close_parser(header, unused)) {
        if (document.builder) {
          last_finish = now;
          co_yield cast(document.builder->finish(), document.target_schema);
          document = {};
        }
        continue;
      }
      // For all header other than #close, we should not have an existing
      // builder anymore. If that's the case then we have a bug in the data,
      // but we can just handle that gracefully and tell the user that they
      // were missing a closing tag.
      if (document.builder) {
        last_finish = now;
        co_yield document.builder->finish();
        document = {};
      }
      // Now we can actually assemble the header.
      // clang-format off
      const auto header_parser
        = ("separator" >> ignore(+parsers::space) >> unescaped_str)
            .with([](std::string separator) {
              return separator.length() == 1;
            })
            .then([&](std::string separator) {
              document.separator = separator[0];
            })
        | ("set_separator" >> separator >> unescaped_str)
            .then([&](std::string set_separator) {
              document.set_separator = std::move(set_separator);
            })
        | ("empty_field" >> separator >> unescaped_str)
            .then([&](std::string empty_field) {
              document.empty_field = std::move(empty_field);
            })
        | ("unset_field" >> separator >> unescaped_str)
            .then([&](std::string unset_field) {
              document.unset_field = std::move(unset_field);
            })
        | ("path" >> separator >> unescaped_str)
            .then([&](std::string path) {
              document.path = std::move(path);
            })
        | ("open" >> separator >> unescaped_str)
            .then([&](std::string open) {
              // This contains a timestamp of the format YYYY-DD-MM-hh-mm-ss
              // that we currently ignore.
              (void)open;
            })
        | ("fields" >> separator >> (unescaped_str % separator))
            .then([&](std::vector<std::string> fields) {
              document.fields = std::move(fields);
            })
        | ("types" >> separator >> (unescaped_str % separator))
            .then([&](std::vector<std::string> types) {
              document.types = std::move(types);
            });
      // clang-format on
      if (not header_parser(header, unused)) {
        diagnostic::warning("invalid Zeek header: {}", *line)
          .note("line {}", line_nr)
          .emit(ctrl.diagnostics());
      }
      continue;
    }
    // If we don't have a builder yet, then we create one lazily.
    if (not document.builder) {
      // We parse the header into three things:
      // 1. A schema that we create the builder with.
      // 2. A rule that parses lines according to the schema.
      if (document.path.empty()) {
        diagnostic::error("failed to parse Zeek document: missing #path")
          .note("line {}", line_nr)
          .emit(ctrl.diagnostics());
        ctrl.self().quit(ec::parse_error);
        co_return;
      }
      if (document.fields.empty()) {
        diagnostic::error("failed to parse Zeek document: missing #fields")
          .note("line {}", line_nr)
          .emit(ctrl.diagnostics());
        ctrl.self().quit(ec::parse_error);
        co_return;
      }
      if (document.fields.size() != document.types.size()) {
        diagnostic::error("failed to parse Zeek document: mismatching number "
                          "#fields and #types")
          .note("found {} #fields", document.fields.size())
          .note("found {} #types", document.types.size())
          .note("line {}", line_nr)
          .emit(ctrl.diagnostics());
        ctrl.self().quit(ec::parse_error);
        co_return;
      }
      // Now we create the schema and the parser rule.
      document.parsers.reserve(document.fields.size());
      auto record_fields = std::vector<record_type::field_view>{};
      record_fields.reserve(document.fields.size());
      for (const auto& [field, zeek_type] :
           detail::zip(document.fields, document.types)) {
        auto parsed_type = parse_type(zeek_type);
        if (not parsed_type) {
          diagnostic::warning("failed to parse Zeek type `{}`", zeek_type)
            .note("line {}", line_nr)
            .note("falling back to `string", line_nr)
            .emit(ctrl.diagnostics());
          parsed_type = type{string_type{}};
        }
        const auto make_unset_parser = [&]() {
          return ignore(parsers::str{document.unset_field}
                        >> &(parsers::chr{document.separator} | parsers::eoi))
            .then([&]() {
              return document.builder->add(caf::none);
            });
        };
        const auto make_empty_parser
          = [&]<concrete_type Type>(const Type& type) {
              return ignore(parsers::str{document.empty_field} >> &(
                              parsers::chr{document.separator} | parsers::eoi))
                .then([&]() {
                  return document.builder->add(type.construct());
                });
            };
        auto make_field_parser =
          [&]<concrete_type Type>(
            const Type& type) -> rule<std::string_view::const_iterator, bool> {
          return make_unset_parser() | make_empty_parser(type)
                 | zeek_parser<Type>{}(type, document.separator,
                                       std::is_same_v<Type, list_type>
                                         ? document.set_separator
                                         : std::string{})
                     .then([&](type_to_data_t<Type> value) {
                       return document.builder->add(value);
                     });
        };
        document.parsers.push_back(caf::visit(make_field_parser, *parsed_type));
        record_fields.push_back({field, std::move(*parsed_type)});
      }
      const auto schema_name = fmt::format("zeek.{}", document.path);
      auto schema = type{schema_name, record_type{record_fields}};
      document.builder = table_slice_builder{std::move(schema)};
      // If there is a schema with the exact matching name, then we set it as a
      // target schema and use that for casting.
      // TODO: This should just unflatten instead.
      auto target_schema = std::find_if(
        ctrl.schemas().begin(), ctrl.schemas().end(), [&](const auto& schema) {
          for (const auto& name : schema.names()) {
            if (name == schema_name) {
              return true;
            }
          }
          return false;
        });
      if (target_schema != ctrl.schemas().end()
          and can_cast(document.builder->schema(), *target_schema)) {
        document.target_schema = *target_schema;
      } else {
        document.target_schema = document.builder->schema();
      }
      // We intentionally fall through here; we create the builder lazily
      // when we encounter the first event, but that we still need to parse
      // now.
    }
    // Lastly, we can apply our rules and parse the builder.
    auto f = line->begin();
    const auto l = line->end();
    auto add_ok = false;
    const auto separator = ignore(parsers::chr{document.separator});
    for (size_t i = 0; i < document.parsers.size() - 1; ++i) {
      const auto parse_ok = document.parsers[i](f, l, add_ok);
      if (not parse_ok) [[unlikely]] {
        diagnostic::error("failed to parse Zeek value at index {} in `{}`", i,
                          *line)
          .note("line {}", line_nr)
          .emit(ctrl.diagnostics());
        ctrl.self().quit(ec::parse_error);
        co_return;
      }
      VAST_ASSERT_EXPENSIVE(add_ok);
      const auto separator_ok = separator(f, l, unused);
      if (not separator_ok) [[unlikely]] {
        diagnostic::error("failed to parse Zeek separator at index {} in `{}`",
                          i, *line)
          .note("line {}", line_nr)
          .emit(ctrl.diagnostics());
        ctrl.self().quit(ec::parse_error);
        co_return;
      }
    }
    const auto parse_ok = document.parsers.back()(f, l, add_ok);
    if (not parse_ok) [[unlikely]] {
      diagnostic::error("failed to parse Zeek value at index {} in `{}`",
                        document.parsers.size() - 1, *line)
        .note("line {}", line_nr)
        .emit(ctrl.diagnostics());
      ctrl.self().quit(ec::parse_error);
      co_return;
    }
    const auto eoi_ok = parsers::eoi(f, l, unused);
    if (not eoi_ok) [[unlikely]] {
      diagnostic::warning("unparsed values at end of Zeek line: `{}`",
                          std::string_view{f, l})
        .note("line {}", line_nr)
        .emit(ctrl.diagnostics());
    }
  }
  if (document.builder->rows() > 0) {
    co_yield cast(document.builder->finish(), document.target_schema);
  }
}

class zeek_tsv_parser final : public plugin_parser {
public:
  auto name() const -> std::string override {
    return "zeek-tsv";
  }

  auto
  instantiate(generator<chunk_ptr> input, operator_control_plane& ctrl) const
    -> std::optional<generator<table_slice>> override {
    return parser_impl(to_lines(std::move(input)), ctrl);
  }

  friend auto inspect(auto&, zeek_tsv_parser&) -> bool {
    return true;
  }
};

class zeek_tsv_printer final : public plugin_printer {
public:
  struct args {
    std::optional<char> set_sep;
    std::optional<std::string> empty_field;
    std::optional<std::string> unset_field;
    bool disable_timestamp_tags = false;

    friend auto inspect(auto& f, args& x) -> bool {
      return f.object(x).fields(
        f.field("set_sep", x.set_sep), f.field("empty_field", x.empty_field),
        f.field("unset_field", x.unset_field),
        f.field("disable_timestamp_tags", x.disable_timestamp_tags));
    }
  };

  zeek_tsv_printer() = default;

  explicit zeek_tsv_printer(args a) : args_{std::move(a)} {
  }

  auto
  instantiate([[maybe_unused]] type input_schema, operator_control_plane&) const
    -> caf::expected<std::unique_ptr<printer_instance>> override {
    auto printer = zeek_printer{args_.set_sep.value_or(','),
                                args_.empty_field.value_or("(empty)"),
                                args_.unset_field.value_or("-"),
                                args_.disable_timestamp_tags};
    return printer_instance::make([printer = std::move(printer)](
                                    table_slice slice) -> generator<chunk_ptr> {
      auto buffer = std::vector<char>{};
      auto out_iter = std::back_inserter(buffer);
      auto resolved_slice = flatten(resolve_enumerations(slice)).slice;
      auto input_schema = resolved_slice.schema();
      auto input_type = caf::get<record_type>(input_schema);
      auto array
        = to_record_batch(resolved_slice)->ToStructArray().ValueOrDie();
      auto first = true;
      for (const auto& row : values(input_type, *array)) {
        VAST_ASSERT_CHEAP(row);
        if (first) {
          printer.print_header(out_iter, input_schema);
          first = false;
          out_iter = fmt::format_to(out_iter, "\n");
        }
        const auto ok = printer.print_values(out_iter, *row);
        VAST_ASSERT_CHEAP(ok);
        out_iter = fmt::format_to(out_iter, "\n");
      }
      printer.print_closing_line(out_iter);
      auto chunk = chunk::make(std::move(buffer));
      co_yield std::move(chunk);
    });
  }

  auto allows_joining() const -> bool override {
    return false;
  }

  auto name() const -> std::string override {
    return "zeek-tsv";
  }

  friend auto inspect(auto& f, zeek_tsv_printer& x) -> bool {
    return f.apply(x.args_);
  }

private:
  args args_;
};

class plugin final : public virtual parser_plugin<zeek_tsv_parser>,
                     public virtual printer_plugin<zeek_tsv_printer> {
public:
  auto name() const -> std::string override {
    return "zeek-tsv";
  }

  auto parse_parser(parser_interface& p) const
    -> std::unique_ptr<plugin_parser> override {
    argument_parser{"zeek-tsv", "https://docs.tenzir.com/next/formats/zeek-tsv"}
      .parse(p);
    return std::make_unique<zeek_tsv_parser>();
  }

  auto parse_printer(parser_interface& p) const
    -> std::unique_ptr<plugin_printer> override {
    auto args = zeek_tsv_printer::args{};
    auto set_separator = std::optional<located<std::string>>{};
    auto parser = argument_parser{"zeek-tsv", "https://docs.tenzir.com/next/"
                                              "formats/zeek-tsv"};
    parser.add("-s,--set-separator", set_separator, "<sep>");
    parser.add("-e,--empty-field", args.empty_field, "<str>");
    parser.add("-u,--unset-field", args.unset_field, "<str>");
    parser.add("-d,--disable-timestamp-tags", args.disable_timestamp_tags);
    parser.parse(p);
    if (set_separator) {
      auto converted = to_xsv_sep(set_separator->inner);
      if (!converted) {
        diagnostic::error("`{}` is not a valid separator", set_separator->inner)
          .primary(set_separator->source)
          .note(fmt::to_string(converted.error()))
          .throw_();
      }
      if (*converted == '\t') {
        diagnostic::error("the `\\t` separator is not allowed here",
                          set_separator->inner)
          .primary(set_separator->source)
          .throw_();
      }
      args.set_sep = *converted;
    }
    return std::make_unique<zeek_tsv_printer>(std::move(args));
  }

  auto initialize(const record&, const record&) -> caf::error override {
    return caf::none;
  }
};

} // namespace

} // namespace vast::plugins::zeek_tsv

VAST_REGISTER_PLUGIN(vast::plugins::zeek_tsv::plugin)
