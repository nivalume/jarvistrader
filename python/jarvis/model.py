"""nautilus-compatible model types (docs/architecture.md section 6).

Everything here is a value: objects are copied across the C++ boundary, never shared. Prices,
quantities and money are exact decimals at 10^9 scale; decimals such as funding rates and margins
are Python ``decimal.Decimal``. Floats enter only through an explicit precision
(``Price(0.1, 2)``, ``Price.from_float(x, 2)``), rounded half to even.

Event classes take their fields positionally or by keyword in the order of ``Cls.fields``; the
names match the event log schema and nautilus_trader.
"""

from ._core import model as _native

__all__ = sorted(name for name in dir(_native) if not name.startswith("_"))
globals().update({name: getattr(_native, name) for name in __all__})
