"""Model types from Python: values, identifiers, data and events."""

from __future__ import annotations

import copy
import pickle
from decimal import Decimal

import pytest

from jarvis import model as m

BTC = m.InstrumentId.from_str("BTCUSDT-PERP.BINANCE")


def trade(**overrides: object) -> m.TradeTick:
    fields: dict[str, object] = {
        "instrument_id": BTC,
        "price": m.Price("65000.1"),
        "size": m.Quantity("0.010"),
        "aggressor_side": m.AggressorSide.SELL,
        "trade_id": m.TradeId("42"),
        "ts_event": 1_700_000_000_000_000_000,
        "ts_init": 1_700_000_000_000_000_500,
    }
    fields.update(overrides)
    return m.TradeTick(**fields)


class TestPrice:
    def test_text_infers_precision_and_keeps_trailing_zeros(self) -> None:
        p = m.Price("1.230")
        assert (p.raw, p.precision, str(p), repr(p)) == (1_230_000_000, 3, "1.230", "Price('1.230')")

    def test_equality_and_ordering_compare_raw_only(self) -> None:
        assert m.Price("1.23") == m.Price("1.230")
        assert hash(m.Price("1.23")) == hash(m.Price("1.230"))
        assert m.Price("0.9") < m.Price("1.0") <= m.Price("1.00")

    def test_explicit_precision_rounds_half_to_even(self) -> None:
        assert str(m.Price("50000.15", 1)) == "50000.2"
        assert str(m.Price("50000.25", 1)) == "50000.2"
        assert str(m.Price(Decimal("1.005"), 2)) == "1.00"

    def test_float_needs_a_precision_and_quantizes_its_shortest_text(self) -> None:
        with pytest.raises(TypeError):
            m.Price(0.1)
        assert str(m.Price(0.1 + 0.2, 2)) == "0.30"
        assert str(m.Price.from_float(2.675, 2)) == "2.68"  # repr(2.675) == "2.675", half to even

    def test_errors_raise_value_error_with_the_status(self) -> None:
        with pytest.raises(ValueError, match="ParseError"):
            m.Price("1.2.3")
        with pytest.raises(ValueError, match="OutOfRange"):
            m.Price("9223372037")
        with pytest.raises(ValueError, match="OutOfRange"):
            m.Quantity("-1")

    def test_arithmetic_and_conversions(self) -> None:
        assert str(m.Price("1.5") + m.Price("0.25")) == "1.75"
        assert str(m.Quantity("1") - m.Quantity("0.25")) == "0.75"
        assert m.Price("1.50").as_decimal() == Decimal("1.50")
        assert m.Price.from_raw(1_500_000_000, 1) == m.Price("1.5")

    def test_values_pickle_and_copy(self) -> None:
        for value in (m.Price("-0.5"), m.Quantity("3"), m.Money("1.5", "USDT"), BTC):
            assert pickle.loads(pickle.dumps(value)) == value
            assert copy.copy(value) == value


class TestMoneyAndCurrency:
    def test_money_rounds_half_to_even_at_currency_precision(self) -> None:
        assert str(m.Money.from_str("1.005 USD")) == "1.00 USD"
        assert str(m.Money.from_str("1.015 USD")) == "1.02 USD"
        assert str(m.Money("1000", m.Currency.from_str("USDT"))) == "1000.00000000 USDT"
        assert m.Money("2", "USD").currency == m.Currency.from_str("USD")

    def test_builtin_currency_table(self) -> None:
        usd = m.Currency.from_str("USD")
        assert (usd.code, usd.precision, usd.iso4217, usd.name, usd.currency_type) == (
            "USD",
            2,
            840,
            "United States dollar",
            m.CurrencyType.FIAT,
        )
        with pytest.raises(ValueError, match="NotFound"):
            m.Currency.from_str("XYZ")


class TestIdentifiers:
    def test_instrument_id_splits_at_the_last_dot(self) -> None:
        iid = m.InstrumentId.from_str("1.211334112-31570229.BETFAIR")
        assert (str(iid.symbol), str(iid.venue)) == ("1.211334112-31570229", "BETFAIR")
        assert m.InstrumentId("BTCUSDT-PERP", "BINANCE") == BTC

    def test_tagged_identifiers(self) -> None:
        assert m.TraderId("TESTER-001").get_tag() == "001"
        assert m.AccountId("BINANCE-USDM-001").get_issuer() == "BINANCE"
        with pytest.raises(ValueError):
            m.TraderId("NOTAG")

    def test_uuid_is_derived_not_random(self) -> None:
        a = m.UUID4.derive(7, 1, 2)
        assert a == m.UUID4.derive(7, 1, 2) != m.UUID4.derive(7, 1, 3)
        assert m.UUID4(str(a)) == a

    def test_client_order_ids_decode(self) -> None:
        gen = m.ClientOrderIdGenerator("mm01", 3)
        first = gen.next()
        assert str(first) == "mm01-000003-00000001"
        assert m.ClientOrderIdGenerator.decode(first) == ("mm01", 3, 1)


class TestEnums:
    def test_values_and_parsing_follow_nautilus(self) -> None:
        assert m.OrderSide.BUY.value == 1
        assert m.OrderSide.from_str("sell") is m.OrderSide.SELL
        assert m.AggressorSide.from_str("BUYER") is m.AggressorSide.BUY
        with pytest.raises(ValueError):
            m.OrderSide.from_str("NO_ORDER_SIDE")


class TestEvents:
    def test_positional_and_keyword_construction_agree(self) -> None:
        t = trade()
        positional = m.TradeTick(*(getattr(t, name) for name in m.TradeTick.fields))
        assert positional == t
        assert t.to_dict()["price"] == m.Price("65000.1")

    def test_missing_unknown_and_mistyped_arguments_are_errors(self) -> None:
        with pytest.raises(TypeError, match="missing required argument 'ts_init'"):
            m.TradeTick(
                instrument_id=BTC,
                price=m.Price("1"),
                size=m.Quantity("1"),
                aggressor_side=m.AggressorSide.BUY,
                trade_id=m.TradeId("1"),
                ts_event=0,
            )
        with pytest.raises(TypeError, match="unexpected"):
            trade(colour="red")
        with pytest.raises(TypeError):
            trade(price=1.5)
        with pytest.raises(ValueError):
            trade(ts_event=-1)

    def test_strings_convert_where_a_value_type_is_expected(self) -> None:
        assert trade(instrument_id="BTCUSDT-PERP.BINANCE", price="65000.1") == trade()

    def test_optionals_default_to_none(self) -> None:
        header = {
            "trader_id": m.TraderId("JARVIS-001"),
            "strategy_id": m.StrategyId("MM-001"),
            "instrument_id": BTC,
            "client_order_id": m.ClientOrderId("mm01-000001-00000001"),
            "event_id": m.UUID4.derive(1, 1, 1),
            "ts_event": 5,
            "ts_init": 6,
        }
        canceled = m.OrderCanceled(**header)
        assert canceled.venue_order_id is None
        assert canceled.reconciliation is False
        filled = m.OrderFilled(
            **header,
            venue_order_id=m.VenueOrderId("1"),
            account_id=m.AccountId("BINANCE-001"),
            trade_id=m.TradeId("9"),
            order_side=m.OrderSide.BUY,
            order_type=m.OrderType.LIMIT,
            last_qty=m.Quantity("0.001"),
            last_px=m.Price("65000.0"),
            currency=m.Currency.from_str("USDT"),
            liquidity_side=m.LiquiditySide.MAKER,
            info_flags=0,
        )
        assert filled.commission is None
        assert filled.trader_id == m.TraderId("JARVIS-001")

    def test_invariants_are_checked(self) -> None:
        bar_type = m.BarType.from_str("BTCUSDT-PERP.BINANCE-1-MINUTE-LAST-EXTERNAL")
        with pytest.raises(ValueError):
            m.Bar(bar_type, m.Price("2"), m.Price("1"), m.Price("1"), m.Price("1"), m.Quantity("1"), 0, 0)
        usdt = m.Currency.from_str("USDT")
        with pytest.raises(ValueError):
            m.AccountBalance(m.Money("10", usdt), m.Money("1", usdt), m.Money("1", usdt))
        assert m.AccountBalance(m.Money("2", usdt), m.Money("1", usdt), m.Money("1", usdt)).currency == usdt

    def test_events_are_values(self) -> None:
        t = trade()
        other = trade()
        other.price = m.Price("1")
        assert t != other
        assert pickle.loads(pickle.dumps(t)) == t
        assert hash(t) == hash(trade())
        assert "price=Price('65000.1')" in repr(t)

    def test_decimals_are_python_decimals(self) -> None:
        f = m.FundingRateUpdate(BTC, Decimal("-0.0001"), 480, 10, 1, 2)
        assert f.rate == Decimal("-0.0001")
        assert isinstance(f.rate, Decimal)

    def test_bar_type_round_trips(self) -> None:
        text = "BTCUSDT-PERP.BINANCE-2-MINUTE-LAST-INTERNAL@1-MINUTE-EXTERNAL"
        bt = m.BarType.from_str(text)
        assert str(bt) == text and bt.is_composite() and bt.is_internally_aggregated()
        assert str(bt.spec) == "2-MINUTE-LAST"

    def test_order_book_deltas_own_their_deltas(self) -> None:
        delta = m.OrderBookDelta(
            BTC, m.BookAction.ADD, m.BookOrder(m.OrderSide.BUY, m.Price("1"), m.Quantity("2"), 0), 144, 7, 1, 2
        )
        deltas = m.OrderBookDeltas(BTC, [delta, delta])
        assert (len(deltas), deltas.flags, deltas.sequence) == (2, 144, 7)
        with pytest.raises(ValueError):
            m.OrderBookDeltas(BTC, [])

    def test_instruments_validate_and_compute_notional(self) -> None:
        usdt = m.Currency.from_str("USDT")
        perp = m.CryptoPerpetual(
            id=BTC,
            raw_symbol=m.Symbol("BTCUSDT"),
            base_currency=m.Currency.from_str("BTC"),
            quote_currency=usdt,
            settlement_currency=usdt,
            price_precision=1,
            size_precision=3,
            price_increment=m.Price("0.1"),
            size_increment=m.Quantity("0.001"),
            multiplier=m.Quantity("1"),
            margin_init=Decimal("0.05"),
            margin_maint=Decimal("0.025"),
            ts_event=0,
            ts_init=0,
        )
        assert str(perp.notional_value(m.Quantity("0.010"), m.Price("65000.0"))) == "650.00000000 USDT"
        assert perp.instrument_class == m.InstrumentClass.SWAP
        with pytest.raises(ValueError):
            m.CryptoPerpetual(**{**perp.to_dict(), "margin_init": Decimal("0")})
