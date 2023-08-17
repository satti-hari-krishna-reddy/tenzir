//    _   _____   __________
//   | | / / _ | / __/_  __/     Visibility
//   | |/ / __ |_\ \  / /          Across
//   |___/_/ |_/___/ /_/       Space and Time
//
// SPDX-FileCopyrightText: (c) 2023 The Tenzir Contributors
// SPDX-License-Identifier: BSD-3-Clause

#include "tenzir/series_builder.hpp"

#include "tenzir/arrow_table_slice.hpp"
#include "tenzir/cast.hpp"
#include "tenzir/concept/printable/tenzir/json.hpp"
#include "tenzir/detail/assert.hpp"
#include "tenzir/detail/narrow.hpp"
#include "tenzir/detail/overload.hpp"
#include "tenzir/detail/stable_map.hpp" // IWYU pragma: keep
#include "tenzir/logger.hpp"
#include "tenzir/table_slice_builder.hpp"
#include "tenzir/type.hpp"
#include "tenzir/view.hpp"

#include <arrow/api.h>
#include <caf/sum_type.hpp>

/// The implementation of `series_builder` consists of the following components:
///
/// - `series_builder` is the entry point of the API. It can be moved freely and
///   immediately delegates everything to `series_builder_impl`.
///
/// - `series_builder_impl` is the actual implementation. It is immovable
///   because we capture a pointer to it to communicate type conflicts from
///   lower builders in order to resolve them. It also stores the table slices
///   that were already finished due to type conflicts and contains some
///   additional logic over the inner builder type.
///
/// - `builder_base` is the polymorphic base class for every inner series
///   builder for a static type kind.
///
/// - `typed_builder<Type>` contains the implementation for a concrete type. In
///   particular, this class has a partial specialization for atomic types, and
///   specializations for the list and record type.
///
/// - `conflict_builder` is a special builder that is used for type conflicts
///   that could not be resolved by flushing previous events. It accepts
///   arbitrary data and converts everything to a string.
///
/// - `dynamic_builder` is a wrapper over `std::unique_ptr<builder_base>` with
///   the ability to change its type kind throughout its lifetime by replacing
///   the inner builder with a different one.
///
///
/// The following diagram summarizes the relation between all components.
///
///                               series_builder
///                                     |
///                                  (wraps)
///                                     |
///                            series_builder_impl
///                               |           ^
///                            (wraps)    (conflict)
///                               |           ^
///                              dynamic_builder < (contains)
///                                     |              ^
///                                  (stores)          ^
///                                     |              ^
///                                builder_base        ^
///                               /     |      \       ^
///                            (is)    (is)    (is)    ^            
///                            /        |        \     ^
///      typed_builder<int64_type>     ...      typed_builder<record_type>
///
///
/// The `dynamic_builder` upgrades the typed builder when a conflict arises.
/// To this end, we use the following lattice.
///
///                           confict_builder
///                            /     |     \.
///     typed_builder<int64_type>   ...    typed_builder<string_type>
///                            \     |     /
///                       typed_builder<null_type>
///
///
/// Because we try to avoid a transition to `conflict_builder`, we finish the
/// series builder for the previous elements when a conflict arises. As long as
/// the type conflict is not caused by a list inside the current top-level
/// series element, this resolves it. In the rare case where finishing does not
/// suffice, we perform the upgrade to a `conflict_builder`.

namespace tenzir {

namespace {

void check(const arrow::Status& status) {
  TENZIR_ASSERT_CHEAP(status.ok(), status.ToString().c_str());
}

} // namespace

namespace detail {

class builder_base {
public:
  virtual ~builder_base() = default;

  virtual auto finish_and_leave(int64_t count) -> typed_array = 0;

  virtual auto arrow_type() const -> std::shared_ptr<arrow::DataType> = 0;

  virtual auto kind() const -> type_kind = 0;

  virtual auto type() const -> type = 0;

  virtual auto length() const -> int64_t = 0;

  virtual auto only_null() const -> bool = 0;

  /// @note If this removes elements, it can be very expensive.
  virtual void resize(int64_t new_length) = 0;
};

template <class Type>
class typed_builder;

template <>
class typed_builder<null_type> final : public detail::builder_base {
public:
  explicit typed_builder(series_builder_impl* root) {
    (void)root;
  }

  auto finish_and_leave(int64_t count) -> typed_array override {
    TENZIR_ASSERT_CHEAP(count <= length_);
    auto builder = arrow::NullBuilder{};
    check(builder.AppendNulls(length_ - count));
    length_ = count;
    return {null_type{}, builder.Finish().ValueOrDie()};
  }

  auto arrow_type() const -> std::shared_ptr<arrow::DataType> override {
    return arrow::null();
  }

  auto kind() const -> type_kind override {
    return tag_v<null_type>;
  }

  auto type() const -> tenzir::type override {
    return tenzir::type{null_type{}};
  }

  auto length() const -> int64_t override {
    return length_;
  }

  void resize(int64_t new_length) override {
    length_ = new_length;
  }

  auto only_null() const -> bool override {
    return true;
  }

private:
  int64_t length_ = 0;
};

/// A type-erased `typed_builder` that can dynamically change its type.
///
/// Additionally, this class stores the metadata if the builder was initialized
/// with a type at the beginning.
class dynamic_builder {
public:
  explicit dynamic_builder(series_builder_impl* root)
    : root_{root}, builder_{std::make_unique<typed_builder<null_type>>(root)} {
  }

  void resize(int64_t new_length) {
    builder_->resize(new_length);
  }

  void atom(atom_view value);

  auto record() -> record_ref;

  auto list() -> builder_ref;

  auto length() const -> int64_t {
    return builder_->length();
  }

  auto arrow_type() const -> std::shared_ptr<arrow::DataType> {
    return builder_->arrow_type();
  }

  auto type() const -> type {
    auto ty = builder_->type();
    ty.assign_metadata(metadata_);
    return ty;
  }

  auto kind() const -> type_kind {
    return builder_->kind();
  }

  auto is_protected() const -> bool {
    return protected_;
  }

  /// Finishes the builder, but leaves the last `count` element. The builder is
  /// fully finished if `count == 0`. We attempt to reduce the underlying type:
  /// If all remaining elements of the series are null, then we transition this
  /// builder the null type. If the series contains records where one field is
  /// always null, we drop the field. If the series contains lists which only
  /// have null items, the inner list type becomes null. Type reduction is also
  /// applied recursively. The goal is to leave the builder in the same state
  /// as-if the last `count` items were added to a fresh builder.
  auto finish_and_leave(int64_t count) -> typed_array {
    TENZIR_INFO("series builder got request to finish but leave {}", count);
    TENZIR_ASSERT_CHEAP(count <= builder_->length());
    auto result = typed_array{};
    if (count == builder_->length()) {
      result.type = type();
      result.array
        = result.type.make_arrow_builder(arrow::default_memory_pool())
            ->Finish()
            .ValueOrDie();
    } else {
      result = builder_->finish_and_leave(count);
    }
    result.type.assign_metadata(metadata_);
    TENZIR_ASSERT_CHEAP(builder_->length() == count);
    if (builder_->only_null() and not is_protected()) {
      TENZIR_DEBUG("reset builder of length {} due to all null of type {}",
                   count, builder_->type());
      builder_ = std::make_unique<typed_builder<null_type>>(root_);
      builder_->resize(count);
    }
    return result;
  }

  /// @pre may only be called once and directly after construction
  void protect(const tenzir::type& ty);

private:
  template <concrete_type Type>
  auto prepare() -> detail::typed_builder<Type>*;

  friend class detail::typed_builder<record_type>;

  // Keeping a pointer here is fine because the type is not movable.
  series_builder_impl* root_;

  /// ...
  std::unique_ptr<detail::builder_base> builder_;

  /// ...
  bool protected_ = false;

  ///
  tenzir::type metadata_;
};

class series_builder_impl {
public:
  series_builder_impl() : builder_{this} {
  }
  ~series_builder_impl() = default;
  series_builder_impl(const series_builder_impl&) = delete;
  series_builder_impl(series_builder_impl&&) = delete;
  auto operator=(const series_builder_impl&) -> series_builder_impl& = delete;
  auto operator=(series_builder_impl&&) -> series_builder_impl& = delete;

  void atom(atom_view value) {
    finish_if_conflict();
    builder_.atom(std::move(value));
  }

  auto record() -> record_ref {
    finish_if_conflict();
    return builder_.record();
  }

  auto list() -> builder_ref {
    finish_if_conflict();
    return builder_.list();
  }

  auto total_length() -> int64_t {
    auto total = builder_.length();
    for (auto& array : finished_) {
      total += array.length();
    }
    return total;
  }

  auto kind() const -> type_kind {
    return builder_.kind();
  }

  auto type() const -> type {
    return builder_.type();
  }

  auto is_protected() const -> bool {
    return builder_.is_protected();
  }

  void finish_previous_events(dynamic_builder* requester) {
    if (builder_.length() == 0) {
      return;
    }
    auto keep_last = true;
    if (requester == &builder_) {
      // This function is called directly when a conflict is detected, before
      // new data is added. Hence, if the requester is the root dynamic builder,
      // then the "current" event was not appended yet, hence we do not keep the
      // last event.
      keep_last = false;
    }
    if (builder_.length() == 1 && keep_last) {
      return;
    }
    auto leave = keep_last ? 1 : 0;
    auto slice = builder_.finish_and_leave(leave);
    TENZIR_ASSERT_CHEAP(builder_.length() == leave);
    TENZIR_ASSERT_CHEAP(slice.length() > 0);
    finished_.push_back(std::move(slice));
  }

  /// Called by `dynamic_builder` if a `conflict_builder` was created.
  void set_conflict_flag() {
    has_conflict_ = true;
  }

  auto finish() -> std::vector<typed_array> {
    has_conflict_ = false;
    if (builder_.length() > 0) {
      finished_.push_back(builder_.finish_and_leave(0));
      TENZIR_ASSERT_CHEAP(builder_.length() == 0);
    }
    return std::exchange(finished_, {});
  }

  void remove_last() {
    has_conflict_ = false;
    if (builder_.length() > 0) {
      builder_.resize(builder_.length() - 1);
    }
  }

  /// @pre may only be called once and directly after construction
  void protect(const tenzir::type& ty) {
    TENZIR_ASSERT_CHEAP(total_length() == 0);
    builder_.protect(ty);
  }

private:
  void finish_if_conflict() {
    if (has_conflict_) {
      finished_.push_back(builder_.finish_and_leave(0));
      has_conflict_ = false;
    }
  }

  dynamic_builder builder_;
  std::vector<typed_array> finished_;

  /// TODO: We finish the builder before we upgrade to a conflict builder.
  /// However, we do not want to keep the conflict builder if the next top-level
  /// series element does not require it. We currently use this flag here to
  /// therefore finish the builder when the next event is started. This can be
  /// very inefficient for inputs where the conflict builder is often used. We
  /// could improve this in the future.
  bool has_conflict_ = false;
};

class conflict_builder final : public detail::builder_base {
public:
  conflict_builder(series_builder_impl* root,
                   std::unique_ptr<detail::builder_base> builder)
    : root_{root} {
    for (auto i = int64_t{0}; i < builder->length(); ++i) {
      discriminants_.push_back(0);
    }
    variants_.push_back(std::move(builder));
  }

  auto finish_and_leave(int64_t count) -> typed_array override {
    TENZIR_WARN("conflict_builder::finish_and_leave({}) with {} variants and "
                "length {}",
                count, variants_.size(), length());
    TENZIR_ASSERT_CHEAP(count <= length());
    auto builder = arrow::StringBuilder{};
    auto variant_counts = std::vector<int64_t>{};
    variant_counts.resize(variants_.size());
    for (auto it = discriminants_.rbegin();
         it != discriminants_.rbegin() + count; ++it) {
      variant_counts[*it] += 1;
    }
    auto variant_arrays = std::vector<typed_array>{};
    variant_arrays.reserve(variants_.size());
    for (auto i = size_t{0}; i < variants_.size(); ++i) {
      variant_arrays.push_back(
        variants_[i]->finish_and_leave(variant_counts[i]));
    }
    TENZIR_WARN("conflict_builder done with collecting data");
    auto variant_offsets = std::vector<int64_t>{};
    variant_offsets.resize(variants_.size());
    for (auto i = int64_t{0}; i < length() - count; ++i) {
      auto discriminant = discriminants_[i];
      TENZIR_ASSERT_CHEAP(discriminant < variants_.size());
      auto offset = variant_offsets[discriminant];
      TENZIR_ASSERT_CHEAP(offset < variant_arrays[discriminant].length());
      auto printer = json_printer{{
        .style = no_style(),
        // TODO: We probably only want to omit null fields.
        // fields. .omit_nulls = true,
        .oneline = true,
      }};
      auto string = std::string{};
      auto out = std::back_inserter(string);
      auto success = printer.print(
        out, value_at(variant_arrays[discriminant].type,
                      *variant_arrays[discriminant].array, offset));
      TENZIR_ASSERT(success);
      check(builder.Append(string));
      variant_offsets[discriminant] += 1;
    }
    discriminants_.erase(discriminants_.begin(), discriminants_.end() - count);
    TENZIR_WARN("returning from conflict_builder::finish_and_leave");
    return {string_type{}, builder.Finish().ValueOrDie()};
  }

  auto arrow_type() const -> std::shared_ptr<arrow::DataType> override {
    return std::make_shared<arrow::StringType>();
  }

  auto kind() const -> type_kind override {
    return tag_v<string_type>;
  }

  auto type() const -> tenzir::type override {
    return tenzir::type{string_type{}};
  }

  auto length() const -> int64_t override {
    return detail::narrow<int64_t>(discriminants_.size());
  }

  auto only_null() const -> bool override {
    for (const auto& variant : variants_) {
      if (not variant->only_null()) {
        return false;
      }
    }
    return true;
  }

  void resize(int64_t new_length) override {
    if (new_length > length()) {
      auto nulls = new_length - length();
      variants_[0]->resize(variants_[0]->length() + nulls);
      discriminants_.insert(discriminants_.end(), nulls, 0);
    } else if (new_length < length()) {
      auto counts = std::vector<int64_t>(variants_.size(), 0);
      auto begin = discriminants_.begin() + new_length;
      while (begin != discriminants_.end()) {
        counts[*begin] += 1;
      }
      discriminants_.erase(begin, discriminants_.end());
      for (auto discr = size_t{0}; discr < counts.size(); ++discr) {
        auto& variant = *variants_[discr];
        variant.resize(variant.length() - counts[discr]);
      }
    }
  }

  template <concrete_type Type>
  auto prepare() -> detail::typed_builder<Type>* {
    static_assert(not std::same_as<Type, null_type>);
    using Target = detail::typed_builder<Type>;
    for (auto& variant : variants_) {
      if (auto* cast = dynamic_cast<Target*>(variant.get())) {
        discriminants_.push_back(
          detail::narrow<uint8_t>(&variant - variants_.data()));
        return cast;
      }
    }
    auto builder = std::make_unique<Target>(root_);
    auto result = builder.get();
    discriminants_.push_back(detail::narrow<uint8_t>(variants_.size()));
    variants_.push_back(std::move(builder));
    return result;
  }

private:
  series_builder_impl* root_;
  std::vector<uint8_t> discriminants_;
  std::vector<std::unique_ptr<detail::builder_base>> variants_;
};

template <class T>
  requires basic_type<T> || std::same_as<T, enumeration_type>
auto append_array_slice(auto& builder, T type, const arrow::Array& array,
                        int64_t offset, int64_t length) {
  using arrow_type = type_to_arrow_type_t<T>;
  using array_type = arrow::TypeTraits<arrow_type>::ArrayType;
  if constexpr (arrow::is_extension_type<arrow_type>::value) {
    // TODO: `AppendArraySlice(...)` throws a `std::bad_cast` with extension
    // types (Arrow 13.0.0). Hence, we have to use some custom logic here.
    auto slice
      = std::static_pointer_cast<array_type>(array.Slice(offset, length));
    for (auto view : values(type, *slice)) {
      if (view) {
        check(append_builder(type, builder, *view));
      } else {
        check(builder.AppendNull());
      }
    }
  } else {
    check(builder.AppendArraySlice(*array.data(), offset, length));
  }
}

template <class T>
  requires((basic_type<T> && not std::same_as<T, null_type>)
           || std::same_as<T, enumeration_type>)
class typed_builder<T> final : public detail::builder_base {
public:
  explicit typed_builder(series_builder_impl* root)
    requires basic_type<T>
    : inner_{T::make_arrow_builder(arrow::default_memory_pool())}, type_{T{}} {
    (void)root;
  }

  explicit typed_builder(T type)
    requires std::same_as<T, enumeration_type>
    : inner_{type.make_arrow_builder(arrow::default_memory_pool())},
      type_{std::move(type)} {
  }

  using builder_type = arrow::TypeTraits<typename T::arrow_type>::BuilderType;
  using value_type = type_to_data_t<T>;
  using view_type = view<value_type>;
  using array_type = arrow::TypeTraits<typename T::arrow_type>::ArrayType;

  auto finish() -> std::shared_ptr<array_type> {
    return std::static_pointer_cast<array_type>(inner_->Finish().ValueOrDie());
  }

  auto finish_and_leave(int64_t count) -> typed_array override {
    auto result = finish();
    append_array_slice(*inner_, type_, *result, result->length() - count,
                       count);
    return {type_, result->SliceSafe(0, result->length() - count).ValueOrDie()};
  }

  auto arrow_type() const -> std::shared_ptr<arrow::DataType> override {
    return type_.to_arrow_type();
  }

  auto kind() const -> type_kind override {
    return tag_v<T>;
  }

  auto type() const -> tenzir::type override {
    return tenzir::type{type_};
  }

  auto length() const -> int64_t override {
    return inner_->length();
  }

  void resize(int64_t new_length) override {
    auto current = length();
    if (current < new_length) {
      check(inner_->AppendNulls(new_length - current));
    } else if (current > new_length) {
      // TODO: This could be optimized, but we do not deem it necessary right now.
      auto array = finish();
      append_array_slice(*inner_, type_, *array, 0, new_length);
    }
  }

  void append(view_type value) {
    check(append_builder(type_, *inner_, value));
  }

  auto only_null() const -> bool override {
    return inner_->null_count() == inner_->length();
  }

private:
  std::shared_ptr<builder_type> inner_;
  T type_;
};

template <>
class typed_builder<list_type> final : public builder_base {
public:
  explicit typed_builder(series_builder_impl* root) : elements_{root} {
  }

  auto append() -> builder_ref {
    check(offsets_.Append(detail::narrow<int32_t>(elements_.length())));
    return builder_ref{elements_};
  }

  void resize(int64_t new_length) override {
    if (new_length < length()) {
      auto offsets = std::shared_ptr<arrow::Int32Array>{};
      check(offsets_.Finish(&offsets));
      check(offsets_.AppendArraySlice(*offsets->data(), 0, new_length));
      // Resize inner value builder to match ending offset of last list element.
      elements_.resize(offsets->Value(new_length));
    } else if (new_length > length()) {
      auto count = new_length - offsets_.length();
      // To append `count` nulls to the list, we have to expand the validity
      // bitmap of the list accordingly. However, the offsets must always be
      // monotonically increasing. This also applies to null offset. We thus
      // have to insert offsets with a certain value, but at the same time, the
      // value must be null. The implementation `AppendValues()` method does
      // this. TODO: We might rely on an implementation detail here.
      auto offset = {elements_.length()};
      auto validity = {false};
      while (count > 0) {
        check(offsets_.AppendValues(offset.begin(), offset.end(),
                                    validity.begin()));
        count -= 1;
      }
    }
  }

  auto finish_and_leave(int64_t count) -> typed_array override {
    auto finish_count = length() - count;
    TENZIR_INFO("list got request to finish {} and leave {}", finish_count,
                count);
    check(offsets_.Append(detail::narrow<int32_t>(elements_.length())));
    auto offsets = std::shared_ptr<arrow::Int32Array>{};
    check(offsets_.Finish(&offsets));
    if (count == 0) {
      TENZIR_ASSERT_CHEAP(offsets->length() == finish_count + 1);
    }
    auto result_offsets = std::static_pointer_cast<arrow::Int32Array>(
      offsets->SliceSafe(0, finish_count + 1).ValueOrDie());
    auto ending_offset = result_offsets->Value(finish_count);
    TENZIR_INFO("ending offset of list is {} of {}", ending_offset,
                elements_.length());
    if (count == 0) {
      TENZIR_ASSERT_CHEAP(ending_offset == elements_.length());
    }
    TENZIR_ASSERT_CHEAP(count == offsets->length() - finish_count - 1);
    // Copy and shift the remaining offsets.
    for (auto i = int64_t{0}; i < count; ++i) {
      auto shifted = offsets->Value(finish_count + i) - ending_offset;
      if (i == 0) {
        TENZIR_ASSERT_CHEAP(shifted == 0);
      }
      check(offsets_.Append(shifted));
    }
    auto remaining_elements = elements_.length() - ending_offset;
    // The following call will reset the list type (and therefore destroy the
    // inner builder) if no elements remain.
    auto used_elements = elements_.finish_and_leave(remaining_elements);
    auto result
      = arrow::ListArray::FromArrays(*result_offsets, *used_elements.array)
          .ValueOrDie();
    TENZIR_ASSERT_EXPENSIVE(result->ValidateFull().ok());
    return {list_type{used_elements.type}, result};
  }

  auto arrow_type() const -> std::shared_ptr<arrow::DataType> override {
    return std::make_shared<arrow::ListType>(elements_.arrow_type());
  }

  auto kind() const -> type_kind override {
    return tag_v<list_type>;
  }

  auto type() const -> tenzir::type override {
    return tenzir::type{list_type{elements_.type()}};
  }

  auto length() const -> int64_t override {
    return offsets_.length();
  }

  auto only_null() const -> bool override {
    return offsets_.null_count() == offsets_.length();
  }

  auto elements_builder() -> dynamic_builder& {
    return elements_;
  }

private:
  /// This only stores beginning indices. Missing ending offsets are added right
  /// before the builder is finished.
  arrow::Int32Builder offsets_;

  dynamic_builder elements_;
};

template <>
class typed_builder<record_type> final : public builder_base {
  friend class detail::field_ref;

public:
  explicit typed_builder(series_builder_impl* root) : root_{root} {
  }

  auto append() -> record_ref {
    length_ += 1;
    return record_ref{this};
  }

  auto finish_and_leave(int64_t count) -> typed_array override {
    TENZIR_ASSERT_CHEAP(count <= length_);
    auto ty = type();
    auto children_arrays = std::vector<std::shared_ptr<arrow::Array>>{};
    children_arrays.reserve(fields_.size());
    auto target_length = length_ - count;
    TENZIR_INFO("finishing {} records with {} fields", target_length,
                fields_.size());
    for (auto it = fields_.begin(); it != fields_.end();) {
      auto& [name, builder] = *it;
      TENZIR_ASSERT_CHEAP(builder->length() <= length_);
      if (builder->length() < target_length) {
        builder->resize(target_length);
      }
      auto leave = builder->length() - target_length;
      TENZIR_INFO("requesting field {} of length {} to finish and leave {}",
                  name, builder->length(), leave);
      auto array = builder->finish_and_leave(leave);
      TENZIR_ASSERT_CHEAP(builder->length() == leave);
      TENZIR_ASSERT_CHEAP(array.length() == target_length);
      children_arrays.push_back(std::move(array.array));
      auto remove = false;
      if (builder->length() > 0) {
        TENZIR_DEBUG("not removing field `{}` due to length {}", name,
                     builder->length());
      } else {
        if (builder->is_protected()) {
          TENZIR_DEBUG("not removing field `{}` due to protection", name);
        } else if (builder.get() == keep_alive_) {
          TENZIR_DEBUG("not removing field `{}` due to keep alive", name);
        } else {
          remove = true;
        }
      }
      if (remove) {
        TENZIR_DEBUG("removing field `{}`", name);
        it = fields_.erase(it);
      } else {
        ++it;
      }
    }
    auto null_bitmap = std::shared_ptr<arrow::Buffer>{};
    if (valid_.length() > 0) {
      if (target_length >= valid_.length()) {
        auto add_trues = target_length - valid_.length();
        check(valid_.Reserve(add_trues));
        while (add_trues > 0) {
          valid_.UnsafeAppend(true);
          add_trues -= 1;
        }
        null_bitmap = valid_.Finish().ValueOrDie();
      } else {
        // TODO: Can this happen? If so, we have to copy the remainder of the
        // validity bitmap.
        TENZIR_TODO();
      }
    }
    auto result
      = std::make_shared<arrow::StructArray>(ty.to_arrow_type(), target_length,
                                             children_arrays,
                                             std::move(null_bitmap));
    TENZIR_ASSERT_EXPENSIVE(result->ValidateFull().ok());
    length_ = count;
    return {std::move(ty), result};
  }

  auto arrow_type() const -> std::shared_ptr<arrow::DataType> override {
    return std::make_shared<arrow::StructType>(make_fields());
  }

  auto kind() const -> type_kind override {
    return tag_v<record_type>;
  }

  auto type() const -> tenzir::type override {
    auto fields = std::vector<struct record_type::field>{};
    fields.reserve(fields_.size());
    for (auto& field : fields_) {
      fields.emplace_back(field.first, field.second->type());
    }
    return tenzir::type{record_type{fields}};
  }

  auto length() const -> int64_t override {
    return length_;
  }

  void resize(int64_t new_length) override {
    if (new_length < length_) {
      if (valid_.length() > new_length) {
        auto nulls = valid_.Finish().ValueOrDie();
        check(valid_.Append(nulls->data(), new_length));
      }
      for (auto& [_, builder] : fields_) {
        if (builder->length() > new_length) {
          builder->resize(new_length);
        }
      }
    } else if (new_length > length_) {
      // Missing values in the validity bitmap are considered to be true, hence
      // we append a corresponding number of true values.
      auto true_count = length_ - valid_.length();
      // However, since we increase the length, we must insert nulls (false).
      auto false_count = new_length - length_;
      check(valid_.Reserve(true_count + false_count));
      while (true_count > 0) {
        valid_.UnsafeAppend(true);
        true_count -= 1;
      }
      while (false_count > 0) {
        valid_.UnsafeAppend(false);
        false_count -= 1;
      }
    }
    length_ = new_length;
  }

  auto only_null() const -> bool override {
    // Missing valdity entries are considered to be true, i.e., non-null.
    return valid_.false_count() == length_;
  }

  /// Prepares field for overwriting (i.e., erases value if already set).
  template <concrete_type Type>
  auto prepare(std::string_view name) -> detail::typed_builder<Type>* {
    static_assert(not std::same_as<Type, null_type>);
    static_assert(not std::same_as<Type, enumeration_type>);
    auto it = fields_.find(name);
    if (it == fields_.end()) {
      auto* builder = insert_new_field(std::string{name});
      builder->resize(length_ - 1);
      return builder->prepare<Type>();
    }
    auto* builder = it->second.get();
    builder->resize(length_ - 1);
    // We temporarily force the field to stay alive. This is because, in the
    // event of a type conflict, the builder will finish the previous events. At
    // the same time, we use this to garbage-collect fields that only contain
    // nulls afterwards. Instead of this flag, we could also detect whether
    // `name` was removed from `builders_` and recreate it if necessary. The
    // effect should be equivalent.
    TENZIR_ASSERT_CHEAP(keep_alive_ == nullptr);
    keep_alive_ = builder;
    auto result = builder->prepare<Type>();
    keep_alive_ = nullptr;
    return result;
  }

  /// @pre Field does not exist yet.
  auto insert_new_field(std::string name) -> dynamic_builder* {
    auto [it, inserted] = fields_.emplace(
      std::move(name), std::make_unique<dynamic_builder>(root_));
    TENZIR_ASSERT_CHEAP(inserted);
    return it->second.get();
  }

private:
  auto make_fields() const -> std::vector<std::shared_ptr<arrow::Field>> {
    auto fields = std::vector<std::shared_ptr<arrow::Field>>{};
    fields.reserve(fields_.size());
    for (auto& [name, builder_] : fields_) {
      fields.push_back(arrow::field(name, builder_->arrow_type()));
    }
    return fields;
  }

  auto builder(std::string_view name) -> dynamic_builder* {
    auto it = fields_.find(name);
    if (it == fields_.end()) {
      return nullptr;
    }
    return it->second.get();
  }

  /// Missing values in fields shall be considered null.
  ///
  /// We have to use `unique_ptr` here because a type conflict might occur in
  /// one of our fields. If this happens, we finish previous events and erase
  /// all fields that only contain null values. While the active field is kept
  /// alive (see `keep_alive_`), this can erase other fields from the map.
  detail::stable_map<std::string, std::unique_ptr<dynamic_builder>> fields_;

  /// Missing values shall be considered true.
  arrow::TypedBufferBuilder<bool> valid_;

  /// Number of records (including nulls) in this builder.
  int64_t length_ = 0;

  /// Used to keep a field builder alive during conflcit flushing.
  dynamic_builder* keep_alive_ = nullptr;

  series_builder_impl* root_;
};

void dynamic_builder::atom(atom_view value) {
  std::visit(
    [&](auto value) {
      if constexpr (std::is_same_v<decltype(value), caf::none_t>) {
        resize(length() + 1);
      } else if constexpr (std::is_same_v<decltype(value), enumeration>) {
        if (auto* cast = dynamic_cast<detail::typed_builder<enumeration_type>*>(
              builder_.get())) {
          cast->append(value);
        } else {
          TENZIR_ASSERT_CHEAP(false, "attempted to add enum data to a non-enum "
                                     "builder");
        }
      } else {
        prepare<atom_view_to_type_t<decltype(value)>>()->append(value);
      }
    },
    value);
}

auto dynamic_builder::record() -> record_ref {
  return prepare<record_type>()->append();
}

auto dynamic_builder::list() -> builder_ref {
  return prepare<list_type>()->append();
}

template <concrete_type Type>
auto dynamic_builder::prepare() -> detail::typed_builder<Type>* {
  static_assert(not std::same_as<Type, null_type>);
  static_assert(not std::same_as<Type, enumeration_type>);
  using Target = detail::typed_builder<Type>;
  TENZIR_ASSERT(builder_ != nullptr);
  if (auto* cast = dynamic_cast<Target*>(builder_.get())) {
    // The most common case: We are already building objects of this type.
    return cast;
  }
  if (auto* cast
      = dynamic_cast<detail::typed_builder<null_type>*>(builder_.get())) {
    // Only happens for the first non-null top-level item: Upgrade the builder.
    auto length = builder_->length();
    builder_ = std::make_unique<Target>(root_);
    builder_->resize(length);
    return static_cast<Target*>(builder_.get());
  }
  if (auto* cast = dynamic_cast<detail::conflict_builder*>(builder_.get())) {
    // This builder is in conflict mode because the current event contains a
    // type conflict.
    return cast->prepare<Type>();
  }
  // Otherwise, there is a type conflict. This means that we have to flush the
  // top-level `series_builder2` and create a new one that is initialized with
  // the data already parsed for the top-level item. However, if the type
  // conflict arises because we are inside a list with conflicting types, this
  // would not solve the situation. Because, as of the time of writing this,
  // we have no sum types, we decided to solve this by converting all values
  // of the conflicting type to a string. Note that this is not necessarily
  // the top-level list item, e.g., if the list contains two records with
  // conflicts.

  auto have_kind = builder_->kind();
  auto want_kind = type_kind::make<Type>();
  TENZIR_ASSERT(have_kind != want_kind);
  TENZIR_ASSERT_CHEAP(not protected_,
                      fmt::format("type mismatch for prepared type: "
                                  "expected {} but got {}",
                                  want_kind, have_kind)
                        .c_str());
  TENZIR_INFO("finishing events due to conflict: requested {} but got {}",
              want_kind, have_kind);

  // TODO: Rewrite and unify the following comments.

  // {"foo": [1, 2]}
  // {"foo": ["hello", "world"]}
  // {"foo": [1, 2]}
  // {"foo": [42, "world"]} -> {"foo": ["42", "world"]}

  // Otherwise, there are three cases to consider:
  //
  // 1. ~~~
  //    {"foo": {"bar": 42}}
  //    {"foo": {"bar": {"baz": 43}}}
  //    ~~~
  //    Here, we are not inside a list and can therefore resolve the conflict
  //    by finishing the previous events.
  //
  // 2. ~~~
  //    {"foo": [{"bar": 1}]}
  //    {"foo": [{"bar": "baz"}]}
  //    ~~~
  //    In this case, we are inside a list, but the conflict is only with
  //    previous events, not within the current event itself. Again, we can
  //    resolve it by finishing the previous events when encountering the
  //    string. This will leave the builder for `foo[].bar` without any data.
  //
  // 3. ~~~
  //    {"foo": [{"bar": 1}, {"bar": "baz"}]}
  //    ~~~
  //    Now the conflict is within the current event. We can therefore not
  //    resolve it properly without introducing sum types. Because, at the
  //    time of writing this, we did not want to do it, we convert the
  //    conflicting items to strings. This also resolves the conflict, at the
  //    cost of an inacurracy with regard to the actual data:
  //    ~~~
  //    {"foo": [{"bar": "1"}, {"bar": "baz"}]}
  //    ~~~
  //    We can differentiate between (2) and (3) by finishing the previous
  //    events, which we have to do anyway. If data remains in the builder,
  //    then we know that there is a conflict within the current event.

  // {"foo": [{"bar": [{"baz": 42}]}, {"bar": [{"baz": "hi"}]}]}
  // {"foo": [{"bar": [{"baz": "42"}]}, {"bar": [{"baz": "hi"}]}]}
  // foo[].bar[].baz

  root_->finish_previous_events(this);

  if (length() > 0) {
    builder_
      = std::make_unique<detail::conflict_builder>(root_, std::move(builder_));
    root_->set_conflict_flag();
  } else {
    TENZIR_ASSERT_CHEAP(builder_->kind().is<null_type>());
  }
  return prepare<Type>();
}

} // namespace detail

void detail::field_ref::atom(atom_view value) {
  std::visit(
    [&](auto value) {
      using atom_type = atom_view_to_type_t<decltype(value)>;
      if constexpr (std::same_as<atom_type, null_type>) {
        // If the value is `null`, we have to handle it differently.
        if (auto* field = origin_->builder(name_)) {
          // We already incremented the length of the record when `.record()`
          // was called. Therefore, If the field was already set (to a non-null
          // value), then the length of the field builder is equal to the length
          // of the record builder. In that case, we remove the last element.
          TENZIR_ASSERT_CHEAP(field->length() <= origin_->length());
          if (field->length() == origin_->length()) {
            field->resize(origin_->length() - 1);
          }
        } else {
          // If the builder does not exist, we do nothing. This means that if a
          // field is always null, it is never added to the record.
        }
      } else if constexpr (std::same_as<atom_type, enumeration_type>) {
        // The `enumeration` type cannot be added on-demand because the value
        // itself does not provide the necessary information to deduce its
        // enumeration type. Thus, we have to special case this.
        auto* builder = origin_->builder(name_);
        if (not builder) {
          TENZIR_ASSERT_CHEAP(false, "cannot get enumeration builder for "
                                     "non-existing field");
        }
        builder->resize(origin_->length_ - 1);
        builder->atom(value);
      } else {
        origin_->prepare<atom_type>(name_)->append(value);
      }
    },
    value);
}

auto detail::field_ref::record() -> record_ref {
  return origin_->prepare<record_type>(name_)->append();
}

auto detail::field_ref::list() -> builder_ref {
  return origin_->prepare<list_type>(name_)->append();
}

auto detail::field_ref::builder() -> detail::dynamic_builder* {
  return origin_->builder(name_);
}

auto detail::field_ref::kind() -> type_kind {
  if (auto* b = builder()) {
    return b->kind();
  }
  return tag_v<null_type>;
}

auto detail::field_ref::type() -> tenzir::type {
  if (auto* b = builder()) {
    return b->type();
  }
  return tenzir::type{null_type{}};
}

auto detail::field_ref::is_protected() -> bool {
  if (auto* b = builder()) {
    return b->is_protected();
  }
  return false;
}

template <class F>
auto builder_ref::dispatch(F&& f) -> decltype(auto) {
  return std::visit(
    [&](auto& ref) {
      if constexpr (std::is_pointer_v<std::remove_cvref_t<decltype(ref)>>) {
        return std::forward<F>(f)(*ref);
      } else {
        return std::forward<F>(f)(ref);
      }
    },
    ref_);
}

void builder_ref::atom(atom_view value) {
  dispatch([&](auto& ref) {
    ref.atom(value);
  });
}

auto builder_ref::try_atom(atom_view value) -> caf::expected<void> {
  if (not is_protected()) {
    if (std::holds_alternative<enumeration>(value)) {
      // We cannot infer the `enumeration_type` from an `enumeration` value.
      return caf::make_error(ec::type_clash, "cannot add enumeration to a "
                                             "non-protected builder");
    }
    atom(value);
    return {};
  }
  auto f = [&]<class FromData, class ToType>(
             const FromData& value, tag<ToType>) -> caf::expected<void> {
    using FromType = atom_view_to_type_t<FromData>;
    using ToData = type_to_data_t<ToType>;
    if constexpr (not detail::atom_type<ToType>) {
      return caf::make_error(ec::type_clash,
                             fmt::format("expected {} but got {}",
                                         type_kind::of<ToType>,
                                         type_kind::of<FromType>));
    } else {
      auto result = caf::expected<ToData>{ToData{}};
      auto full_ty = type();
      auto ty = caf::get<ToType>(full_ty);
      // TODO: Consider refactoring this logic.
      if constexpr (std::same_as<ToType, duration_type>) {
        // TODO: Should we prefer to error if no unit was specified?
        auto unit = full_ty.attribute("unit").value_or("s");
        if constexpr (
          // TODO: These special cases were extracted from `cast.hpp`.
          // We should make it so that this is not necessary.
          std::same_as<FromType, int64_type>
          || std::same_as<FromType, uint64_type>
          || std::same_as<FromType, double_type>) {
          result = cast_value(FromType{}, value, ty, unit);
        } else if constexpr (std::same_as<FromType, string_type>) {
          result = cast_value(FromType{}, value, ty);
          if (not result) {
            result
              = cast_value(FromType{}, fmt::format("{} {}", value, unit), ty);
          }
        } else {
          result = cast_value(FromType{}, value, ty);
        }
      } else {
        result = cast_value(FromType{}, value, ty);
      }
      if (not result) {
        return result.error();
      }
      atom(*result);
      return {};
    }
  };
  return std::visit(f, value, kind());
}

void builder_ref::data(data_view2 value) {
  auto result = try_data(std::move(value));
  TENZIR_ASSERT_CHEAP(result, fmt::to_string(result.error()).c_str());
}

auto builder_ref::try_data(data_view2 value) -> caf::expected<void> {
  auto f = detail::overload{
    [&](const view<tenzir::record>& x) -> caf::expected<void> {
      auto r = record();
      for (auto&& [name, data] : x) {
        return r.field(name).try_data(data);
      }
      return {};
    },
    [&](const view<tenzir::list>& x) -> caf::expected<void> {
      auto l = list();
      for (const auto& y : x) {
        return l.try_data(y);
      }
      return {};
    },
    [&](const auto& x) -> caf::expected<void> {
      return try_atom(x);
    },
    [&](const view<pattern>&) -> caf::expected<void> {
      TENZIR_UNREACHABLE();
    },
    [&](const view<map>&) -> caf::expected<void> {
      TENZIR_UNREACHABLE();
    },
  };
  return std::visit(f, value);
}

auto builder_ref::list() -> builder_ref {
  return dispatch([&](auto& ref) {
    return ref.list();
  });
}

auto builder_ref::record() -> record_ref {
  return dispatch([&](auto& ref) {
    return ref.record();
  });
}

auto builder_ref::kind() -> type_kind {
  return dispatch([&](auto& ref) {
    return ref.kind();
  });
}

auto builder_ref::type() -> tenzir::type {
  return dispatch([&](auto& ref) {
    return ref.type();
  });
}

auto builder_ref::is_protected() -> bool {
  return dispatch([&](auto& ref) {
    return ref.is_protected();
  });
}

series_builder::series_builder(
  std::optional<std::reference_wrapper<const tenzir::type>> ty)
  : impl_{std::make_unique<detail::series_builder_impl>()} {
  if (ty) {
    impl_->protect(*ty);
  }
}

series_builder::~series_builder() = default;

series_builder::series_builder(series_builder&&) noexcept = default;

auto series_builder::operator=(series_builder&&) noexcept
  -> series_builder& = default;

void series_builder::null() {
  impl_->atom(caf::none);
}

void series_builder::atom(atom_view value) {
  impl_->atom(value);
}

void series_builder::data(data_view2 value) {
  builder_ref{*this}.data(std::move(value));
}

auto series_builder::record() -> record_ref {
  return impl_->record();
}

auto series_builder::list() -> builder_ref {
  return impl_->list();
}

auto series_builder::finish() -> std::vector<typed_array> {
  return impl_->finish();
}

auto series_builder::finish_as_table_slice(std::string_view name)
  -> std::vector<table_slice> {
  auto arrays = finish();
  auto result = std::vector<table_slice>{};
  result.reserve(arrays.size());
  for (auto& array : arrays) {
    TENZIR_ASSERT_CHEAP(caf::holds_alternative<record_type>(array.type));
    TENZIR_ASSERT_CHEAP(array.length() > 0);
    if (not name.empty()) {
      // The following check is not an optimization, but prevents
      // double-wrapping, which would change `#schema_id`.
      if (name != array.type.name()) {
        array.type = tenzir::type{name, array.type};
      }
    } else if (array.type.name().empty()) {
      // Previously, we used `array.type.make_fingerprint()` as the schema name
      // here. However, the name is included as part of the fingerprint, which
      // means that `#schema` was a different fingerprint than `#schema_id`,
      // which creates potential for confusion.
      // TODO: Consider not using a name here at all.
      array.type = tenzir::type{"tenzir.json", array.type};
    }
    auto* cast = dynamic_cast<arrow::StructArray*>(array.array.get());
    TENZIR_ASSERT_CHEAP(cast);
    auto arrow_schema = array.type.to_arrow_schema();
    auto batch = arrow::RecordBatch::Make(std::move(arrow_schema),
                                          cast->length(), cast->fields());
    TENZIR_ASSERT_CHEAP(batch);
    TENZIR_ASSERT_EXPENSIVE(batch->ValidateFull().ok());
    result.emplace_back(batch, array.type);
  }
  return result;
}

auto series_builder::kind() -> type_kind {
  return impl_->kind();
}

auto series_builder::type() -> tenzir::type {
  return impl_->type();
}

auto series_builder::length() -> int64_t {
  return impl_->total_length();
}

void series_builder::remove_last() {
  impl_->remove_last();
}

void detail::dynamic_builder::protect(const tenzir::type& ty) {
  TENZIR_ASSERT_CHEAP(kind().is<null_type>());
  TENZIR_ASSERT_CHEAP(length() == 0);
  metadata_ = ty;
  protected_ = true;
  auto f = detail::overload{
    [&](const null_type&) {
      // Do nothing, as we already are a null builder.
    },
    [&]<basic_type T>(const T&) {
      prepare<T>();
    },
    [&](const enumeration_type& ty) {
      builder_ = std::make_unique<detail::typed_builder<enumeration_type>>(ty);
    },
    [&](const record_type& ty) {
      auto* record = prepare<record_type>();
      for (auto&& [name, field_ty] : ty.fields()) {
        auto* field = record->insert_new_field(std::string{name});
        field->protect(field_ty);
      }
    },
    [&](const list_type& ty) {
      auto* list = prepare<list_type>();
      list->elements_builder().protect(ty.value_type());
    },
    [&](const map_type&) {
      TENZIR_UNREACHABLE();
    },
  };
  caf::visit(f, ty);
}

} // namespace tenzir
