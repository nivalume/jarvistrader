# The Python half of jarvis._core.model: the enum classes and the event classes, built from the
# tables the extension provides (_ENUMS, _CLASSES, _DELTAS, _INSTRUMENTS) and installed on the
# native submodule (_native). Executed once when the extension is imported.

import enum as _enum

_MODULE = "jarvis._core.model"


def _make_enum(name, members, doc):
    cls = _enum.Enum(name, [(n, v) for n, v, _ in members], module=_MODULE)
    lookup = {}
    for n, v, aliases in members:
        lookup[n.lower()] = cls(v)
        for a in aliases:
            lookup[a.lower()] = cls(v)

    def from_str(klass, text):
        try:
            return lookup[str(text).lower()]
        except KeyError:
            raise ValueError(f"{name}: {text!r} is not a {name}") from None

    cls.from_str = classmethod(from_str)
    cls.__doc__ = doc
    return cls


def _hashable(value):
    if isinstance(value, list):
        return tuple(_hashable(v) for v in value)
    return value


class _Event:
    """Base of the event classes: positional or keyword construction in the order of ``fields``,
    validation by the kernel, value semantics."""

    fields = ()
    _field_set = frozenset()
    _defaults = {}

    def __init__(self, *args, **kwargs):
        cls = type(self)
        fields = cls.fields
        if len(args) > len(fields):
            raise TypeError(
                f"{cls.__name__}() takes {len(fields)} positional arguments but {len(args)} were given"
            )
        values = dict(zip(fields, args))
        for key, value in kwargs.items():
            if key not in cls._field_set:
                raise TypeError(f"{cls.__name__}() got an unexpected keyword argument {key!r}")
            if key in values:
                raise TypeError(f"{cls.__name__}() got multiple values for argument {key!r}")
            values[key] = value
        for name in fields:
            if name not in values:
                if name in cls._defaults:
                    values[name] = cls._defaults[name]
                else:
                    raise TypeError(f"{cls.__name__}() missing required argument {name!r}")
        self.__dict__.update(values)
        _native._normalize(self)

    def to_dict(self):
        return {name: getattr(self, name) for name in type(self).fields}

    def __eq__(self, other):
        return type(other) is type(self) and self.to_dict() == other.to_dict()

    def __ne__(self, other):
        return not self.__eq__(other)

    def __hash__(self):
        return hash((type(self).__name__, _hashable(list(self.to_dict().values()))))

    def __repr__(self):
        body = ", ".join(f"{k}={v!r}" for k, v in self.to_dict().items())
        return f"{type(self).__name__}({body})"


class _Deltas(_Event):
    def __len__(self):
        return len(self.deltas)


class _Instrument(_Event):
    def notional_value(self, quantity, price):
        """quantity * multiplier * price (inverse: / price), truncated to the currency grid."""
        return _native._notional_value(self, quantity, price)

    @property
    def instrument_class(self):
        return _native._instrument_class(self)

    @property
    def asset_class(self):
        return _native._asset_class(self)


def _make_class(name, doc, fields, base=_Event, extra_attrs=()):
    field_names = tuple(f for f, _ in fields)
    defaults = {f: d for f, d in fields if d is not _MISSING}
    namespace = {
        "__doc__": doc,
        "__module__": _MODULE,
        "fields": field_names,
        "_field_set": frozenset(field_names),
        "_defaults": defaults,
    }
    # Class-level placeholders so that every attribute exists on the class itself.
    for f in field_names + tuple(extra_attrs):
        namespace[f] = None
    cls = type(name, (base,), namespace)
    setattr(_native, name, cls)
    return cls



for _name, _members, _doc in _ENUMS:
    setattr(_native, _name, _make_enum(_name, _members, _doc))

for _name, _doc, _fields in _CLASSES:
    _make_class(_name, _doc, _fields)

_make_class("OrderBookDeltas", _DELTAS[0], _DELTAS[1], base=_Deltas, extra_attrs=_DELTAS[2])
for _name, _doc, _fields in _INSTRUMENTS:
    _make_class(_name, _doc, _fields, base=_Instrument)

_native.AccountBalance.currency = property(lambda self: self.total.currency)
