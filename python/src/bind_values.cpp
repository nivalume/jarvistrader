// Python classes for model value types: Price, Quantity, Money, Currency, the identifiers,
// UUID4, BarSpecification, BarType, BookOrder and the ClientOrderId generator.

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

#include <nanobind/nanobind.h>
#include <nanobind/stl/optional.h>
#include <nanobind/stl/string.h>
#include <nanobind/stl/string_view.h>
#include <nanobind/stl/tuple.h>

#include "common.hpp"
#include "jarvis/core/rng.hpp"
#include "jarvis/model/client_order_id.hpp"
#include "jarvis/model/data.hpp"

namespace jarvis::py {

namespace {

template <typename T> std::string text_of(const T& value) {
  std::array<char, m::kMaxMoneyText> buffer{};
  std::size_t n = 0;
  static_cast<void>(value.format(buffer, n));
  return std::string{buffer.data(), n};
}

std::int64_t hash_text(std::string_view text) {
  return static_cast<std::int64_t>(
      core::crc32c(std::as_bytes(std::span<const char>{text.data(), text.size()})));
}

// Price, Quantity: exact decimal values at 10^9 scale with a display precision (section 6.1).
template <typename T, typename Raw>
void bind_fixed(nb::module_& mod, const char* name, const char* doc) {
  nb::class_<T> cls(mod, name, doc);
  cls.def(
      "__init__",
      [name](T* self, nb::handle value, std::optional<std::uint8_t> precision) {
        if (nb::isinstance<nb::float_>(value) && !precision) {
          throw nb::type_error(
              (std::string{name} + "(): a float needs an explicit precision to be quantized")
                  .c_str());
        }
        const std::string text = decimal_text(value, name);
        T out;
        check(precision ? T::parse(text, *precision, out) : T::parse(text, out), name);
        new (self) T{out};
      },
      nb::arg("value"), nb::arg("precision") = nb::none(),
      "From str, int or decimal.Decimal (precision inferred unless given), or from a float with "
      "an explicit precision. Digits beyond the precision round half to even.");
  cls.def_static(
      "from_str",
      [name](std::string_view text) {
        T out;
        check(T::parse(text, out), name);
        return out;
      },
      nb::arg("text"));
  cls.def_static(
      "from_float",
      [name](nb::handle value, std::uint8_t precision) {
        if (!nb::isinstance<nb::float_>(value) && !nb::isinstance<nb::int_>(value)) {
          type_error(name, "a float or int", value);
        }
        T out;
        check(T::parse(decimal_text(value, name), precision, out), name);
        return out;
      },
      nb::arg("value"), nb::arg("precision"),
      "Quantizes a float to `precision` digits, rounding half to even (section 7.7).");
  cls.def_static(
      "from_raw",
      [name](Raw raw, std::uint8_t precision) {
        T out;
        check(T::from_raw(raw, precision, out), name);
        return out;
      },
      nb::arg("raw"), nb::arg("precision"));
  cls.def_prop_ro("raw", [](const T& v) { return v.raw(); });
  cls.def_prop_ro("precision", [](const T& v) { return v.precision(); });
  cls.def("as_decimal", [](const T& v) { return python_decimal(text_of(v)); });
  cls.def("as_double", [](const T& v) { return std::stod(text_of(v)); });
  cls.def("is_zero", [](const T& v) { return v.is_zero(); });
  cls.def("__str__", [](const T& v) { return text_of(v); });
  cls.def("__repr__", [name](const T& v) { return std::string{name} + "('" + text_of(v) + "')"; });
  cls.def("__eq__", [](const T& a, nb::handle b) {
    T other;
    return nb::try_cast<T>(b, other, false) && a.raw() == other.raw();
  });
  cls.def("__lt__", [](const T& a, const T& b) { return a.raw() < b.raw(); });
  cls.def("__le__", [](const T& a, const T& b) { return a.raw() <= b.raw(); });
  cls.def("__gt__", [](const T& a, const T& b) { return a.raw() > b.raw(); });
  cls.def("__ge__", [](const T& a, const T& b) { return a.raw() >= b.raw(); });
  cls.def("__hash__", [](const T& v) { return static_cast<std::int64_t>(v.raw()); });
  cls.def("__add__", [name](const T& a, const T& b) {
    T out;
    check(T::add(a, b, out), name);
    return out;
  });
  cls.def("__sub__", [name](const T& a, const T& b) {
    T out;
    check(T::sub(a, b, out), name);
    return out;
  });
  cls.def("__getstate__", [](const T& v) { return nb::make_tuple(v.raw(), v.precision()); });
  cls.def("__setstate__", [name](T& self, const nb::tuple& state) {
    T out;
    check(T::from_raw(nb::cast<Raw>(state[0]), nb::cast<std::uint8_t>(state[1]), out), name);
    new (&self) T{out};
  });
}

m::Currency currency_of(nb::handle h) {
  m::Currency c;
  from_py(h, c, "currency");
  return c;
}

void bind_money(nb::module_& mod) {
  nb::class_<m::Money>(mod, "Money", "An amount of a currency on the currency's precision grid.")
      .def(
          "__init__",
          [](m::Money* self, nb::handle amount, nb::handle currency) {
            const m::Currency c = currency_of(currency);
            m::Money out;
            check(m::Money::parse_amount(decimal_text(amount, "Money"), c, out), "Money");
            new (self) m::Money{out};
          },
          nb::arg("amount"), nb::arg("currency"),
          "Amount as str, int, decimal.Decimal or float, rounded half to even at the currency's "
          "precision; currency as a Currency or a built-in code.")
      .def_static(
          "from_str",
          [](std::string_view text) {
            m::Money out;
            check(m::Money::parse(text, out), "Money");
            return out;
          },
          nb::arg("text"))
      .def_static(
          "from_raw",
          [](std::int64_t raw, nb::handle currency) {
            m::Money out;
            check(m::Money::from_raw(raw, currency_of(currency), out), "Money");
            return out;
          },
          nb::arg("raw"), nb::arg("currency"))
      .def_prop_ro("raw", [](const m::Money& v) { return v.raw(); })
      .def_prop_ro("currency", [](const m::Money& v) { return v.currency(); })
      .def("as_decimal",
           [](const m::Money& v) {
             const std::string text = text_of(v);
             return python_decimal(text.substr(0, text.find(' ')));
           })
      .def("as_double",
           [](const m::Money& v) {
             const std::string text = text_of(v);
             return std::stod(text.substr(0, text.find(' ')));
           })
      .def("__str__", [](const m::Money& v) { return text_of(v); })
      .def("__repr__", [](const m::Money& v) { return "Money('" + text_of(v) + "')"; })
      .def("__eq__",
           [](const m::Money& a, nb::handle b) {
             m::Money other;
             return nb::try_cast<m::Money>(b, other, false) && a == other;
           })
      .def("__hash__", [](const m::Money& v) { return hash_text(text_of(v)); })
      .def("__add__",
           [](const m::Money& a, const m::Money& b) {
             m::Money out;
             check(m::Money::add(a, b, out), "Money");
             return out;
           })
      .def("__sub__",
           [](const m::Money& a, const m::Money& b) {
             m::Money out;
             check(m::Money::sub(a, b, out), "Money");
             return out;
           })
      .def("__getstate__", [](const m::Money& v) { return text_of(v); })
      .def("__setstate__", [](m::Money& self, std::string_view text) {
        m::Money out;
        check(m::Money::parse(text, out), "Money");
        new (&self) m::Money{out};
      });
}

void bind_currency(nb::module_& mod) {
  nb::class_<m::Currency>(mod, "Currency", "A currency; equality is by code.")
      .def(
          "__init__",
          [](m::Currency* self, std::string_view code, std::uint8_t precision,
             std::uint16_t iso4217, std::string_view name, m::CurrencyType type) {
            m::Currency out;
            check(m::Currency::create(code, precision, iso4217, name, type, out), "Currency");
            new (self) m::Currency{out};
          },
          nb::arg("code"), nb::arg("precision"), nb::arg("iso4217"), nb::arg("name"),
          nb::arg("currency_type"))
      .def_static(
          "from_str",
          [](std::string_view code) {
            m::Currency out;
            check(m::Currency::builtin(code, out), "Currency");
            return out;
          },
          nb::arg("code"), "A built-in nautilus currency by code.")
      .def_prop_ro("code", [](const m::Currency& c) { return std::string{c.code()}; })
      .def_prop_ro("precision", [](const m::Currency& c) { return c.precision(); })
      .def_prop_ro("iso4217", [](const m::Currency& c) { return c.iso4217(); })
      .def_prop_ro("name", [](const m::Currency& c) { return std::string{c.name()}; })
      .def_prop_ro("currency_type", [](const m::Currency& c) { return c.currency_type(); })
      .def("__str__", [](const m::Currency& c) { return std::string{c.code()}; })
      .def("__repr__",
           [](const m::Currency& c) { return "Currency('" + std::string{c.code()} + "')"; })
      .def("__eq__",
           [](const m::Currency& a, nb::handle b) {
             m::Currency other;
             return nb::try_cast<m::Currency>(b, other, false) && a == other;
           })
      .def("__hash__", [](const m::Currency& c) { return hash_text(c.code()); })
      .def("__getstate__",
           [](const m::Currency& c) {
             return nb::make_tuple(std::string{c.code()}, c.precision(), c.iso4217(),
                                   std::string{c.name()}, c.currency_type());
           })
      .def("__setstate__", [](m::Currency& self, const nb::tuple& s) {
        m::Currency out;
        check(m::Currency::create(nb::cast<std::string>(s[0]), nb::cast<std::uint8_t>(s[1]),
                                  nb::cast<std::uint16_t>(s[2]), nb::cast<std::string>(s[3]),
                                  nb::cast<m::CurrencyType>(s[4]), out),
              "Currency");
        new (&self) m::Currency{out};
      });
}

template <typename Id> nb::class_<Id> bind_id(nb::module_& mod, const char* name, const char* doc) {
  nb::class_<Id> cls(mod, name, doc);
  cls.def(
         "__init__",
         [name](Id* self, std::string_view value) {
           Id out;
           check(Id::from(value, out), name);
           new (self) Id{out};
         },
         nb::arg("value"))
      .def_static(
          "from_str",
          [name](std::string_view value) {
            Id out;
            check(Id::from(value, out), name);
            return out;
          },
          nb::arg("value"))
      .def_prop_ro("value", [](const Id& id) { return std::string{id.view()}; })
      .def("__str__", [](const Id& id) { return std::string{id.view()}; })
      .def(
          "__repr__",
          [name](const Id& id) { return std::string{name} + "('" + std::string{id.view()} + "')"; })
      .def("__eq__",
           [](const Id& a, nb::handle b) {
             Id other;
             return nb::try_cast<Id>(b, other, false) && a == other;
           })
      .def("__lt__", [](const Id& a, const Id& b) { return a.view() < b.view(); })
      .def("__hash__", [](const Id& id) { return hash_text(id.view()); })
      .def("__getstate__", [](const Id& id) { return std::string{id.view()}; })
      .def("__setstate__", [name](Id& self, std::string_view value) {
        Id out;
        check(Id::from(value, out), name);
        new (&self) Id{out};
      });
  return cls;
}

void bind_identifiers(nb::module_& mod) {
  bind_id<m::Symbol>(mod, "Symbol", "Venue symbol of an instrument, e.g. BTCUSDT-PERP.");
  bind_id<m::Venue>(mod, "Venue", "Trading venue, e.g. BINANCE.");
  bind_id<m::TraderId>(mod, "TraderId", "\"{name}-{tag}\".")
      .def("get_tag", [](const m::TraderId& id) { return std::string{m::tag_of(id)}; });
  bind_id<m::StrategyId>(mod, "StrategyId", "\"{name}-{tag}\".")
      .def("get_tag", [](const m::StrategyId& id) { return std::string{m::tag_of(id)}; });
  bind_id<m::AccountId>(mod, "AccountId", "\"{issuer}-{number}\".")
      .def("get_issuer", [](const m::AccountId& id) { return std::string{m::issuer_of(id)}; })
      .def("get_id", [](const m::AccountId& id) {
        const std::string_view text = id.view();
        return std::string{text.substr(text.find('-') + 1)};
      });
  bind_id<m::ClientOrderId>(mod, "ClientOrderId", "Client-assigned order id.");
  bind_id<m::VenueOrderId>(mod, "VenueOrderId", "Venue-assigned order id.");
  bind_id<m::ClientId>(mod, "ClientId", "Data or execution client id.");
  bind_id<m::ComponentId>(mod, "ComponentId", "Component id.");
  bind_id<m::ExecAlgorithmId>(mod, "ExecAlgorithmId", "Execution algorithm id.");
  bind_id<m::OrderListId>(mod, "OrderListId", "Order list id.");
  bind_id<m::TradeId>(mod, "TradeId", "Venue trade id (at most 36 ASCII characters).");
  bind_id<m::PositionId>(mod, "PositionId", "Position id.");

  nb::class_<m::InstrumentId>(mod, "InstrumentId", "\"{symbol}.{venue}\", split at the last dot.")
      .def(
          "__init__",
          [](m::InstrumentId* self, nb::handle symbol, nb::handle venue) {
            m::Symbol s;
            m::Venue v;
            from_py(symbol, s, "symbol");
            from_py(venue, v, "venue");
            m::InstrumentId out;
            check(m::InstrumentId::create(s, v, out), "InstrumentId");
            new (self) m::InstrumentId{out};
          },
          nb::arg("symbol"), nb::arg("venue"))
      .def_static(
          "from_str",
          [](std::string_view text) {
            m::InstrumentId out;
            check(m::InstrumentId::parse(text, out), "InstrumentId");
            return out;
          },
          nb::arg("value"))
      .def_prop_ro("symbol", [](const m::InstrumentId& id) { return id.symbol; })
      .def_prop_ro("venue", [](const m::InstrumentId& id) { return id.venue; })
      .def_prop_ro("value", [](const m::InstrumentId& id) { return std::string{id.text().view()}; })
      .def("__str__", [](const m::InstrumentId& id) { return std::string{id.text().view()}; })
      .def("__repr__",
           [](const m::InstrumentId& id) {
             return "InstrumentId('" + std::string{id.text().view()} + "')";
           })
      .def("__eq__",
           [](const m::InstrumentId& a, nb::handle b) {
             m::InstrumentId other;
             return nb::try_cast<m::InstrumentId>(b, other, false) && a == other;
           })
      .def("__lt__", [](const m::InstrumentId& a, const m::InstrumentId& b) { return a < b; })
      .def("__hash__", [](const m::InstrumentId& id) { return hash_text(id.text().view()); })
      .def("__getstate__", [](const m::InstrumentId& id) { return std::string{id.text().view()}; })
      .def("__setstate__", [](m::InstrumentId& self, std::string_view text) {
        m::InstrumentId out;
        check(m::InstrumentId::parse(text, out), "InstrumentId");
        new (&self) m::InstrumentId{out};
      });

  nb::class_<m::Uuid4>(mod, "UUID4",
                       "RFC 4122 version 4 UUID. There is no random constructor: "
                       "ids come from text or from UUID4.derive (deterministic).")
      .def(
          "__init__",
          [](m::Uuid4* self, std::string_view value) {
            m::Uuid4 out;
            check(m::Uuid4::parse(value, out), "UUID4");
            new (self) m::Uuid4{out};
          },
          nb::arg("value"))
      .def_static(
          "from_str",
          [](std::string_view value) {
            m::Uuid4 out;
            check(m::Uuid4::parse(value, out), "UUID4");
            return out;
          },
          nb::arg("value"))
      .def_static(
          "derive",
          [](std::uint64_t seed, std::uint64_t identity, std::uint32_t hop) {
            return m::Uuid4::derive(core::CounterRng{seed}, identity, hop);
          },
          nb::arg("seed"), nb::arg("identity"), nb::arg("hop"),
          "The UUID drawn from the counter-based RNG at (seed, identity, hop).")
      .def_prop_ro("value",
                   [](const m::Uuid4& u) {
                     const m::Uuid4::Text t = u.text();
                     return std::string{t.data(), t.size()};
                   })
      .def("__str__",
           [](const m::Uuid4& u) {
             const m::Uuid4::Text t = u.text();
             return std::string{t.data(), t.size()};
           })
      .def("__repr__",
           [](const m::Uuid4& u) {
             const m::Uuid4::Text t = u.text();
             return "UUID4('" + std::string{t.data(), t.size()} + "')";
           })
      .def("__eq__",
           [](const m::Uuid4& a, nb::handle b) {
             m::Uuid4 other;
             return nb::try_cast<m::Uuid4>(b, other, false) && a == other;
           })
      .def("__hash__",
           [](const m::Uuid4& u) {
             const m::Uuid4::Text t = u.text();
             return hash_text(std::string_view{t.data(), t.size()});
           })
      .def("__getstate__",
           [](const m::Uuid4& u) {
             const m::Uuid4::Text t = u.text();
             return std::string{t.data(), t.size()};
           })
      .def("__setstate__", [](m::Uuid4& self, std::string_view text) {
        m::Uuid4 out;
        check(m::Uuid4::parse(text, out), "UUID4");
        new (&self) m::Uuid4{out};
      });
}

void bind_bars(nb::module_& mod) {
  bind_struct<m::BarSpecification>(mod, "BarSpecification",
                                   "\"{step}-{AGGREGATION}-{PRICE_TYPE}\".")
      .def_static(
          "from_str",
          [](std::string_view text) {
            m::BarSpecification out;
            check(m::BarSpecification::parse(text, out), "BarSpecification");
            return out;
          },
          nb::arg("value"))
      .def("__str__", [](const m::BarSpecification& s) {
        m::detail::TextBuilder b;
        s.write(b);
        return std::string{b.view()};
      });
  bind_struct<m::BarType>(
      mod, "BarType",
      "\"{instrument_id}-{spec}-{SOURCE}\", optionally \"@{step}-{AGG}-{SOURCE}\" "
      "for a composite bar type.")
      .def_static(
          "from_str",
          [](std::string_view text) {
            m::BarType out;
            check(m::BarType::parse(text, out), "BarType");
            return out;
          },
          nb::arg("value"))
      .def("__str__", [](const m::BarType& t) { return std::string{t.text().view()}; })
      .def("is_externally_aggregated",
           [](const m::BarType& t) {
             return t.aggregation_source == m::AggregationSource::External;
           })
      .def("is_internally_aggregated",
           [](const m::BarType& t) {
             return t.aggregation_source == m::AggregationSource::Internal;
           })
      .def("is_composite", [](const m::BarType& t) { return t.composite; });
  bind_struct<m::BookOrder>(mod, "BookOrder", "One order or price level of an order book.");
}

void bind_client_order_ids(nb::module_& mod) {
  nb::class_<m::ClientOrderIdGenerator>(
      mod, "ClientOrderIdGenerator",
      "\"{node_tag}-{epoch}-{seq}\" client order ids (section 8.4): no wall clock, decodable.")
      .def(
          "__init__",
          [](m::ClientOrderIdGenerator* self, std::string_view node_tag, std::uint64_t epoch) {
            m::ClientOrderIdGenerator out;
            check(m::ClientOrderIdGenerator::create(node_tag, epoch, out),
                  "ClientOrderIdGenerator");
            new (self) m::ClientOrderIdGenerator{out};
          },
          nb::arg("node_tag"), nb::arg("epoch"))
      .def("next",
           [](m::ClientOrderIdGenerator& g) {
             m::ClientOrderId out;
             check(g.next(out), "ClientOrderIdGenerator.next");
             return out;
           })
      .def_static(
          "decode",
          [](nb::handle id) {
            m::ClientOrderId value;
            from_py(id, value, "client_order_id");
            m::DecodedClientOrderId out;
            check(m::ClientOrderIdGenerator::decode(value, out), "ClientOrderIdGenerator.decode");
            return nb::make_tuple(std::string{out.node_tag.view()}, out.epoch, out.seq);
          },
          nb::arg("client_order_id"), "(node_tag, epoch, seq) of a jarvis client order id.");
}

} // namespace

void bind_values(nb::module_& mod) {
  bind_currency(mod);
  bind_fixed<m::Price, std::int64_t>(mod, "Price", "Signed price: raw int64 at 10^9 scale.");
  bind_fixed<m::Quantity, std::uint64_t>(mod, "Quantity",
                                         "Non-negative quantity: raw uint64 at 10^9 scale.");
  bind_money(mod);
  bind_identifiers(mod);
  bind_bars(mod);
  bind_client_order_ids(mod);
}

} // namespace jarvis::py
