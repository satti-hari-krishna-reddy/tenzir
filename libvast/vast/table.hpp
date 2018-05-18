/******************************************************************************
 *                    _   _____   __________                                  *
 *                   | | / / _ | / __/_  __/     Visibility                   *
 *                   | |/ / __ |_\ \  / /          Across                     *
 *                   |___/_/ |_/___/ /_/       Space and Time                 *
 *                                                                            *
 * This file is part of VAST. It is subject to the license terms in the       *
 * LICENSE file found in the top-level directory of this distribution and at  *
 * http://vast.io/license. No part of VAST, including this file, may be       *
 * copied, modified, propagated, or distributed except according to the terms *
 * contained in the LICENSE file.                                             *
 ******************************************************************************/

#pragma once

#include <cstdint>
#include <vector>

#include <caf/intrusive_ptr.hpp>
#include <caf/make_counted.hpp>
#include <caf/optional.hpp>
#include <caf/ref_counted.hpp>

#include "vast/data.hpp"
#include "vast/type.hpp"
#include "vast/view.hpp"

namespace vast {

class table_slice;

/// @relates table_slice
using table_slice_ptr = caf::intrusive_ptr<table_slice>;

/// A dataset in tabular form. A table consists of [@ref table_slice](slices),
/// each of which have the same layout.
/// @relates table_slice
class table {
public:
  using size_type = uint64_t;

  /// Constructs a table with a specific layout.
  /// @param layout The record describing the table columns.
  table(record_type layout);

  /// Adds a slice to the table.
  /// @returns A failure if the layout is not compatible with 
  bool add(table_slice_ptr slice);

  /// Retrieves the table layout.
  const record_type& layout() const;

  /// @returns the number of rows in the table.
  size_type rows() const;

  /// @returns the number of rows in the slice.
  size_type columns() const;

  /// Retrieves data by specifying 2D-coordinates via row and column.
  /// @param row The row offset.
  /// @param col The column offset.
  /// @pre `row < rows() && col < columns()`
  caf::optional<data_view> at(size_type row, size_type col) const;

  /// @returns The slices in this table.
  const std::vector<table_slice_ptr>& slices() const;

private:
  std::vector<table_slice_ptr> slices_;
  record_type layout_;
};

/// A horizontal partition of a table. A slice defines a tabular interface for
/// accessing homogenous data independent of the concrete carrier format.
/// @relates table
class table_slice : public caf::ref_counted {
public:
  class builder;

  /// @relates builder
  using builder_ptr = caf::intrusive_ptr<builder>;

  /// Enables incremental construction of a table slice.
  class builder : public caf::ref_counted {
  public:
    virtual ~builder() = default;

    /// Factory function to construct a builder.
    /// @param layout The layout of the builder.
    /// @returns A reference-counted pointer to a builder.
    template <class T, class... Ts>
    static builder_ptr make(record_type layout, Ts&&... xs) {
      return T::builder::make(std::move(layout), std::forward<Ts>(xs)...);
    }

    /// Adds data to the builder.
    /// @param x The data to add.
    /// @returns `true` on success.
    virtual bool add(data_view x) = 0;

    /// Constructs a table_slice from the currently accumulated state.
    /// @returns A table slice or `nullptr` on failure.
    virtual table_slice_ptr finish() = 0;

  protected:
    builder() = default;
  };

  using size_type = table::size_type;

  virtual ~table_slice() = default;

  /// @returns The table layout.
  const record_type& layout() const;

  /// @returns the number of rows in the slice.
  size_type rows() const;

  /// @returns the number of rows in the slice.
  size_type columns() const;

  /// Retrieves data by specifying 2D-coordinates via row and column.
  /// @param row The row offset.
  /// @param col The column offset.
  /// @pre `row < rows() && col < columns()`
  virtual caf::optional<data_view> at(size_type row, size_type col) const = 0;

protected:
  /// Constructs a table slice with a specific layout.
  /// @param layout The record describing the table columns.
  table_slice(record_type layout);

  record_type layout_; // flattened
  size_type rows_;
  size_type columns_;
};

} // namespace vast
