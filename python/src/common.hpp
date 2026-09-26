#pragma once

// Helpers shared by the binding translation units: Status to Python exceptions, conversions
// between model field types and Python objects, and `bind_struct`, which builds a Python class
// from a model::fields() descriptor so that attribute names equal the event log's field names.

#include <Python.h>

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>
#include <vector>

#include <nanobind/nanobind.h>

#include "jarvis/core/crc32c.hpp"
#include "jarvis/core/fixed_string.hpp"
#include "jarvis/core/status.hpp"
#include "jarvis/core/time.hpp"
#include "jarvis/model/bar.hpp"
#include "jarvis/model/currency.hpp"
#include "jarvis/model/fixed_point.hpp"
#include "jarvis/model/identifiers.hpp"
#include "jarvis/model/money.hpp"
#include "jarvis/model/schema.hpp"
#include "jarvis/model/uuid.hpp"
#include "jarvis/model/wire.hpp"

namespace jarvis::py {

namespace nb = nanobind;
namespace m = jarvis::model;

// ---- errors ----------------------------------------------------------------------------------

// Kernel failures surface as ValueError (IoError as OSError), with the Status name in the text.
[[noreturn]] inline void raise(core::Status status, std::string_view what) {
  const std::string message = std::string{what} + ": " + std::string{core::to_string(status)};
  if (status == core::Status::IoError) {
    PyErr_SetString(PyExc_OSError, message.c_str());
    throw nb::python_error();
  }
  throw nb::value_error(message.c_str());
}

inline void check(core::Status status, std::string_view what) {
  if (!core::ok(status)) {
    raise(status, what);
  }
}

[[noreturn]] inline void type_error(std::string_view field, std::string_view expected,
                                    nb::handle got) {
  const std::string message = std::string{field} + ": expected " + std::string{expected} +
                              ", got " + nb::type_name(got.type()).c_str();
  throw nb::type_error(message.c_str());
}

// ---- traits ----------------------------------------------------------------------------------

template <typename T> struct is_optional : std::false_type {};
template <typename T> struct is_optional<std::optional<T>> : std::true_type {};
template <typename T> struct is_fixed_string : std::false_type {};
template <std::size_t N> struct is_fixed_string<core::FixedString<N>> : std::true_type {};
template <typename T> struct is_std_array : std::false_type {};
template <typename T, std::size_t N> struct is_std_array<std::array<T, N>> : std::true_type {};
template <typename T> struct is_signed_fixed : std::false_type {};
template <typename Tag> struct is_signed_fixed<m::SignedFixed<Tag>> : std::true_type {};

// Model decimals (rates, margins, offsets) cross the boundary as Python decimal.Decimal, as in
// nautilus; prices and quantities keep their own classes.
inline constexpr bool kDecimalAsPython = true;

inline nb::object python_decimal(std::string_view text) {
  static nb::object decimal = nb::module_::import_("decimal").attr("Decimal");
  return decimal(nb::str(text.data(), text.size()));
}

// Text of a Python value used as a decimal: str for str, int and decimal.Decimal; repr for float
// (the shortest string that round-trips, so quantizing it is deterministic).
inline std::string decimal_text(nb::handle h, std::string_view field) {
  if (nb::isinstance<nb::str>(h)) {
    return nb::borrow<nb::str>(h).c_str();
  }
  if (PyBool_Check(h.ptr()) != 0) {
    type_error(field, "a decimal (str, int, Decimal or float)", h);
  }
  if (nb::isinstance<nb::int_>(h)) {
    return nb::str(h).c_str();
  }
  if (nb::isinstance<nb::float_>(h)) {
    return nb::repr(h).c_str();
  }
  static nb::object decimal = nb::module_::import_("decimal").attr("Decimal");
  if (nb::isinstance(h, decimal)) {
    return nb::str(h).c_str();
  }
  type_error(field, "a decimal (str, int, Decimal or float)", h);
}

// ---- parsing from text (lets Python pass "BTCUSDT-PERP.BINANCE" where an id is expected) -----

template <typename T> inline constexpr bool kParsesFromText = false;
template <typename Rule> inline constexpr bool kParsesFromText<m::Identifier<Rule>> = true;
template <> inline constexpr bool kParsesFromText<m::InstrumentId> = true;
template <> inline constexpr bool kParsesFromText<m::Price> = true;
template <> inline constexpr bool kParsesFromText<m::Quantity> = true;
template <> inline constexpr bool kParsesFromText<m::Money> = true;
template <> inline constexpr bool kParsesFromText<m::Currency> = true;
template <> inline constexpr bool kParsesFromText<m::Uuid4> = true;
template <> inline constexpr bool kParsesFromText<m::BarType> = true;
template <> inline constexpr bool kParsesFromText<m::BarSpecification> = true;

template <typename T> core::Status parse_text(std::string_view text, T& out) {
  if constexpr (std::is_same_v<T, m::Currency>) {
    return m::Currency::builtin(text, out);
  } else if constexpr (requires { T::parse(text, out); }) {
    return T::parse(text, out);
  } else {
    return T::from(text, out);
  }
}

// ---- field conversions -----------------------------------------------------------------------

template <typename T> nb::object to_py(const T& v);

template <typename T> nb::object to_py(const T& v) {
  if constexpr (std::is_same_v<T, bool>) {
    return nb::bool_(v);
  } else if constexpr (std::is_enum_v<T>) {
    return nb::cast(v);
  } else if constexpr (std::is_integral_v<T>) {
    return nb::int_(v);
  } else if constexpr (std::is_same_v<T, core::UnixNanos> ||
                       std::is_same_v<T, core::DurationNanos>) {
    return nb::int_(v.value());
  } else if constexpr (is_fixed_string<T>::value) {
    const std::string_view text = v.view();
    return nb::str(text.data(), text.size());
  } else if constexpr (is_optional<T>::value) {
    return v ? to_py(*v) : nb::none();
  } else if constexpr (is_std_array<T>::value) {
    nb::list list;
    for (const auto& item : v) {
      list.append(to_py(item));
    }
    return list;
  } else if constexpr (std::is_same_v<T, m::Decimal>) {
    std::array<char, m::kMaxDecimalText> buffer{};
    std::size_t n = 0;
    static_cast<void>(v.format(buffer, n));
    return python_decimal(std::string_view{buffer.data(), n});
  } else {
    return nb::cast(v, nb::rv_policy::copy);
  }
}

template <typename U> U python_uint(nb::handle h, std::string_view field) {
  if (PyBool_Check(h.ptr()) != 0 || !nb::isinstance<nb::int_>(h)) {
    type_error(field, "an integer", h);
  }
  const unsigned long long value = PyLong_AsUnsignedLongLong(h.ptr());
  if (PyErr_Occurred() != nullptr || value > std::numeric_limits<U>::max()) {
    PyErr_Clear();
    throw nb::value_error((std::string{field} + ": out of range").c_str());
  }
  return static_cast<U>(value);
}

template <typename T> void from_py(nb::handle h, T& out, std::string_view field);

template <typename T> void int_from_py(nb::handle h, T& out, std::string_view field) {
  if constexpr (std::is_unsigned_v<T>) {
    out = python_uint<T>(h, field);
  } else {
    if (PyBool_Check(h.ptr()) != 0 || !nb::isinstance<nb::int_>(h)) {
      type_error(field, "an integer", h);
    }
    const long long value = PyLong_AsLongLong(h.ptr());
    if (PyErr_Occurred() != nullptr || value < std::numeric_limits<T>::min() ||
        value > std::numeric_limits<T>::max()) {
      PyErr_Clear();
      throw nb::value_error((std::string{field} + ": out of range").c_str());
    }
    out = static_cast<T>(value);
  }
}

template <typename T> void optional_from_py(nb::handle h, T& out, std::string_view field) {
  if (h.is_none()) {
    out = std::nullopt;
    return;
  }
  typename T::value_type value{};
  from_py(h, value, field);
  out = value;
}

template <typename T> void array_from_py(nb::handle h, T& out, std::string_view field) {
  if (!nb::isinstance<nb::sequence>(h) || nb::len(h) != out.size()) {
    type_error(field, "a sequence of " + std::to_string(out.size()) + " items", h);
  }
  for (std::size_t i = 0; i < out.size(); ++i) {
    from_py(h[i], out[i], field);
  }
}

// Bound classes; value types also accept their text form.
template <typename T> void object_from_py(nb::handle h, T& out, std::string_view field) {
  if (nb::try_cast<T>(h, out, false)) {
    return;
  }
  if constexpr (kParsesFromText<T>) {
    if (nb::isinstance<nb::str>(h)) {
      check(parse_text(std::string_view{nb::borrow<nb::str>(h).c_str()}, out), field);
      return;
    }
  }
  type_error(field, nb::type_name(nb::type<T>()).c_str(), h);
}

template <typename T> void from_py(nb::handle h, T& out, std::string_view field) {
  if constexpr (std::is_same_v<T, bool>) {
    if (PyBool_Check(h.ptr()) == 0) {
      type_error(field, "a bool", h);
    }
    out = h.ptr() == Py_True;
  } else if constexpr (std::is_enum_v<T>) {
    if (!nb::try_cast<T>(h, out, false)) {
      type_error(field, "an enum member", h);
    }
  } else if constexpr (std::is_integral_v<T>) {
    int_from_py(h, out, field);
  } else if constexpr (std::is_same_v<T, core::UnixNanos> ||
                       std::is_same_v<T, core::DurationNanos>) {
    out = T{python_uint<std::uint64_t>(h, field)};
  } else if constexpr (is_fixed_string<T>::value) {
    if (!nb::isinstance<nb::str>(h)) {
      type_error(field, "a str", h);
    }
    check(T::from(nb::borrow<nb::str>(h).c_str(), out), field);
  } else if constexpr (is_optional<T>::value) {
    optional_from_py(h, out, field);
  } else if constexpr (is_std_array<T>::value) {
    array_from_py(h, out, field);
  } else if constexpr (std::is_same_v<T, m::Decimal>) {
    check(m::Decimal::parse(decimal_text(h, field), out), field);
  } else {
    object_from_py(h, out, field);
  }
}

// ---- schema-driven classes -------------------------------------------------------------------

template <typename T> std::vector<std::string> field_names() {
  std::vector<std::string> names;
  T probe{};
  m::fields(probe, [&names](std::string_view name, auto& /*field*/) { names.emplace_back(name); });
  return names;
}

// Fields a constructor call must provide: everything except optionals and booleans.
template <typename T> std::vector<bool> field_required() {
  std::vector<bool> required;
  T probe{};
  m::fields(probe, [&required](std::string_view /*name*/, auto& field) {
    using F = std::decay_t<decltype(field)>;
    required.push_back(!is_optional<F>::value && !std::is_same_v<F, bool>);
  });
  return required;
}

template <typename T> nb::object get_field(const T& self, std::size_t index) {
  T copy = self;
  nb::object result;
  std::size_t k = 0;
  m::fields(copy, [&](std::string_view /*name*/, const auto& field) {
    if (k++ == index) {
      result = to_py(field);
    }
  });
  return result;
}

template <typename T> void set_field(T& self, std::size_t index, nb::handle value) {
  std::size_t k = 0;
  m::fields(self, [&](std::string_view name, auto& field) {
    if (k++ == index) {
      from_py(value, field, name);
    }
  });
}

// Invariants checked after construction; types without one accept any field values.
template <typename T> core::Status validate_value(const T& /*value*/) { return core::Status::Ok; }
core::Status validate_value(const m::Bar& bar);
core::Status validate_value(const m::CurrencyPair& instrument);
core::Status validate_value(const m::CryptoPerpetual& instrument);
core::Status validate_value(const m::CryptoFuture& instrument);
core::Status validate_value(const m::AccountBalance& balance);

// The wire encoding of a value; equality, hashing and pickling use it.
template <typename T> std::vector<std::byte> encode_value(const T& value) {
  std::vector<std::byte> buffer(4096);
  m::wire::Writer w{buffer};
  m::wire::put_fields(w, value);
  if (!w.ok()) {
    throw nb::value_error("value too large to encode");
  }
  buffer.resize(w.size());
  return buffer;
}

template <typename T> T decode_value(const nb::bytes& bytes, const char* name) {
  const auto* data = static_cast<const std::byte*>(bytes.data());
  m::wire::Reader r{std::span<const std::byte>{data, bytes.size()}};
  T value{};
  m::wire::get_fields(r, value);
  if (!r.ok() || r.remaining() != 0) {
    raise(r.ok() ? core::Status::InvalidArgument : r.status(), name);
  }
  return value;
}

template <typename T> std::string repr_value(const T& value, const char* name) {
  std::string out = std::string{name} + "(";
  T copy = value;
  bool first = true;
  m::fields(copy, [&](std::string_view field_name, const auto& field) {
    out += first ? "" : ", ";
    first = false;
    out += field_name;
    out += "=";
    out += nb::repr(to_py(field)).c_str();
  });
  return out + ")";
}

// Positional arguments fill fields in order, keywords by name; required fields must be given.
template <typename T>
void init_from_arguments(T& self, const nb::args& args, const nb::kwargs& kwargs, const char* name,
                         const std::vector<std::string>& names, const std::vector<bool>& required) {
  if (args.size() > names.size()) {
    throw nb::type_error(
        (std::string{name} + "() takes at most " + std::to_string(names.size()) + " arguments")
            .c_str());
  }
  std::vector<bool> given(names.size(), false);
  for (std::size_t i = 0; i < args.size(); ++i) {
    set_field(self, i, args[i]);
    given[i] = true;
  }
  for (const auto& [key, value] : kwargs) {
    const std::string key_text = nb::str(key).c_str();
    const auto it = std::find(names.begin(), names.end(), key_text);
    const auto index = static_cast<std::size_t>(it - names.begin());
    if (it == names.end() || given[index]) {
      throw nb::type_error(
          (std::string{name} + "() got an unexpected or repeated argument '" + key_text + "'")
              .c_str());
    }
    set_field(self, index, value);
    given[index] = true;
  }
  for (std::size_t i = 0; i < names.size(); ++i) {
    if (required[i] && !given[i]) {
      throw nb::type_error(
          (std::string{name} + "() missing required argument '" + names[i] + "'").c_str());
    }
  }
  check(validate_value(self), name);
}

// Python class for a described model struct: positional or keyword construction in field order,
// one read-write attribute per field, value equality and hashing, repr, to_dict and pickling.
template <typename T>
nb::class_<T> bind_struct(nb::handle scope, const char* name, const char* doc) {
  static const std::vector<std::string> names = field_names<T>();
  static const std::vector<bool> required = field_required<T>();
  nb::class_<T> cls(scope, name, doc);
  cls.def(
      "__init__",
      [name](T* self, const nb::args& args, const nb::kwargs& kwargs) {
        new (self) T{};
        init_from_arguments(*self, args, kwargs, name, names, required);
      },
      doc);
  for (std::size_t i = 0; i < names.size(); ++i) {
    cls.def_prop_rw(
        names[i].c_str(), [i](const T& self) { return get_field(self, i); },
        [i](T& self, nb::handle value) { set_field(self, i, value); });
  }
  cls.def("__eq__", [](const T& a, nb::handle b) {
    T other{};
    if (!nb::try_cast<T>(b, other, false)) {
      return false;
    }
    return encode_value(a) == encode_value(other);
  });
  cls.def("__hash__", [](const T& self) {
    return static_cast<std::int64_t>(core::crc32c(encode_value(self)));
  });
  cls.def("__repr__", [name](const T& self) { return repr_value(self, name); });
  cls.def("to_dict", [](const T& self) {
    nb::dict d;
    for (std::size_t i = 0; i < names.size(); ++i) {
      d[names[i].c_str()] = get_field(self, i);
    }
    return d;
  });
  cls.def("__copy__", [](const T& self) { return self; });
  cls.def("__deepcopy__", [](const T& self, nb::handle /*memo*/) { return self; });
  cls.def("__getstate__", [](const T& self) {
    const std::vector<std::byte> bytes = encode_value(self);
    return nb::bytes(reinterpret_cast<const char*>(bytes.data()), bytes.size()); // NOLINT
  });
  cls.def("__setstate__",
          [name](T& self, const nb::bytes& state) { new (&self) T{decode_value<T>(state, name)}; });
  cls.def_prop_ro_static("fields", [](nb::handle /*cls*/) {
    nb::tuple out = nb::steal<nb::tuple>(PyTuple_New(static_cast<Py_ssize_t>(names.size())));
    for (std::size_t i = 0; i < names.size(); ++i) {
      PyTuple_SET_ITEM(out.ptr(), static_cast<Py_ssize_t>(i), // NOLINT
                       nb::str(names[i].c_str()).release().ptr());
    }
    return out;
  });
  return cls;
}

template <typename E> E parse_enum(std::string_view text, const char* name) {
  E value{};
  check(m::parse(text, value), name);
  return value;
}

// Binding entry points, one per translation unit.
void bind_generated_enums(nb::module_& mod);
void bind_values(nb::module_& mod);
void bind_events(nb::module_& mod);
void bind_log(nb::module_& mod);

} // namespace jarvis::py
