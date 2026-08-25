# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Attribute-access benchmark: one workload per receiver layout.

``LOAD_ATTR`` and ``STORE_ATTR`` are the two opcodes CinderX caches most
aggressively, and how fast they run depends almost entirely on the *layout* of
the receiver -- where the attribute physically lives and what has to be checked
to prove the cached answer is still valid.  This benchmark builds one receiver
per distinct layout that CPython 3.12, 3.14 and 3.15 can produce, then hammers a
generated hot function per layout, so each workload isolates exactly one cache
shape.

Every hot function is generated from the same template and takes its receivers
as an untyped ``list`` parameter.  That matters: with the receiver's type unknown
at compile time the JIT cannot fold the load into a raw field access, so the site
stays on the inline-cache path this benchmark exists to measure.  Storage layouts
covered:

* ``inline`` -- managed dict whose values still live inline in the object.
* ``split`` -- inline values plus a materialized dict sharing the type's keys.
* ``combined`` -- managed-dict instance whose dict was detached and unshared.
* ``exhausted`` -- the type's shared keys are full, so instances carry a real
  dict and the attribute name is absent from the shared keys.
* ``dictoffset`` -- non-managed heap type with a positive ``tp_dictoffset``.
* ``slots`` / ``slotsdict`` -- member descriptors, with and without a dict.
* ``varsize`` -- managed dict on a variable-sized type.  On 3.14+ such types
  never get inline values; on 3.12 they do.
* ``afteritems`` -- ``__slots__`` on a ``tuple`` subclass, whose member offsets
  are only known at runtime.  3.15+ only; skipped elsewhere.

plus the lookup kinds that bypass instance storage entirely: ``classvar``,
``property``, ``descriptor``, ``getset``, ``method``, ``typeattr``, ``module``,
``getattr`` and ``getattribute``.

The ``poly*`` workloads point a single site at several receiver types at once.
CinderX's attribute caches hold ``CINDERX_JIT_ATTR_CACHE_SIZE`` entries (4 by
default) and never evict, so ``poly8`` and ``poly16`` are permanently
megamorphic -- they pay a full scan plus a failed refill on every execution.
``polytype`` thrashes the type-attribute cache, which holds a single entry.

Results are reported in nanoseconds per attribute operation, so workloads with
different receiver counts stay comparable.

Run a workload, or compare the interpreter against the JIT, with::

    attr_cache --workload inline
    attr_cache --compare --workload inline-store
    attr_cache --describe
    attr_cache --specialization --workload split

"""

from __future__ import annotations

import datetime
import dis
import gc
import importlib
import math
import os
import statistics
import subprocess
import sys
import time
import types
from dataclasses import dataclass, field
from typing import Callable

import cinderx.jit
import click


SUBPROCESS_ENV_KEYS: tuple[str, ...] = (
    "HOME",
    "LANG",
    "LC_ALL",
    "LD_LIBRARY_PATH",
    "PATH",
    "PYTHONPATH",
    "TMPDIR",
    "VIRTUAL_ENV",
)

# Attribute names every layout exposes, so all workloads run the same shape of
# hot loop and their numbers stay directly comparable.
NAMES: tuple[str, ...] = ("a0", "a1", "a2", "a3")

METHOD_NAMES: tuple[str, ...] = ("m0", "m1", "m2", "m3")

# Receivers per workload.  Monomorphic workloads repeat one type; polymorphic
# ones cycle through several, so a site sees each type once per pass.
RECEIVERS: int = 8

# Times the name block is repeated in the hot function.  Each repetition is a
# distinct bytecode instruction and therefore a distinct inline cache.
UNROLL: int = 8

STORE_VALUE: int = 7

# Enough distinct attributes to exhaust a type's shared keys, which cap out at
# SHARED_KEYS_MAX_SIZE (30) in all three CPython versions.
SHARED_KEY_FILLER: int = 64

# Py_TPFLAGS bits that determine where instance attributes live.  INLINE_VALUES
# only exists from 3.14 on.
TPFLAGS_INLINE_VALUES: int = 1 << 2
TPFLAGS_MANAGED_DICT: int = 1 << 4


class Unsupported(Exception):
    """Raised by a builder for a layout this Python version cannot produce."""


# ---------------------------------------------------------------------------
# Receiver construction
# ---------------------------------------------------------------------------


def _new_class(
    name: str,
    namespace: dict[str, object] | None = None,
    bases: tuple[type, ...] = (),
) -> type:
    return type(name, bases, dict(namespace or {}))


def _set_names(objs: list[object], names: tuple[str, ...] = NAMES) -> None:
    for obj in objs:
        for i, name in enumerate(names):
            setattr(obj, name, i)


def _build_inline() -> list[object]:
    cls = _new_class("InlineValues")
    objs: list[object] = [cls() for _ in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_split() -> list[object]:
    objs = _build_inline()
    for obj in objs:
        # Materializing __dict__ hands out a dict sharing the type's keys.  On
        # 3.14+ the inline values stay valid and back that dict; on 3.12 the
        # object's tagged union flips to the dict permanently.
        vars(obj)
    return objs


def _build_combined() -> list[object]:
    objs = _build_inline()
    for obj in objs:
        # Assigning a fresh dict detaches the instance from the shared keys.
        obj.__dict__ = dict(vars(obj))
    return objs


def _build_exhausted() -> list[object]:
    cls = _new_class("ExhaustedKeys")
    filler = cls()
    for i in range(SHARED_KEY_FILLER):
        setattr(filler, f"f{i}", i)
    objs: list[object] = [cls() for _ in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_dictoffset() -> list[object]:
    # BaseException has a positive tp_dictoffset, so a heap subclass inherits a
    # plain combined __dict__ at a fixed offset rather than a managed one.
    cls = _new_class("DictOffset", bases=(Exception,))
    objs: list[object] = [cls() for _ in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_slots() -> list[object]:
    cls = _new_class("Slots", {"__slots__": NAMES})
    objs: list[object] = [cls() for _ in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_slots_dict() -> list[object]:
    cls = _new_class("SlotsAndDict", {"__slots__": (*NAMES[:2], "__dict__")})
    objs: list[object] = [cls() for _ in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_varsize() -> list[object]:
    cls = _new_class("VarSize", bases=(tuple,))
    objs: list[object] = [cls((i, i + 1, i + 2)) for i in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_after_items() -> list[object]:
    try:
        cls = _new_class("AfterItems", {"__slots__": NAMES}, bases=(tuple,))
    except TypeError as exc:
        raise Unsupported("__slots__ on a tuple subclass needs 3.15+") from exc
    objs: list[object] = [cls((i, i + 1, i + 2)) for i in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_classvar() -> list[object]:
    cls = _new_class("ClassVar", {name: i for i, name in enumerate(NAMES)})
    return [cls() for _ in range(RECEIVERS)]


def _property_source() -> str:
    """Source for a class whose attributes are all properties backed by slots."""
    lines = [
        "class PropertyReceiver:",
        f"    __slots__ = {tuple('_' + name for name in NAMES)!r}",
    ]
    for name in NAMES:
        lines += [
            "    @property",
            f"    def {name}(self):",
            f"        return self._{name}",
            f"    @{name}.setter",
            f"    def {name}(self, value):",
            f"        self._{name} = value",
        ]
    return "\n".join(lines) + "\n"


def _build_property() -> list[object]:
    namespace: dict[str, object] = {}
    exec(compile(_property_source(), "<attr_cache:property>", "exec"), namespace)
    cls: type = namespace["PropertyReceiver"]  # pyre-ignore[9]
    objs: list[object] = [cls() for _ in range(RECEIVERS)]
    _set_names(objs)
    return objs


class DataDescriptor:
    """A data descriptor that is not a ``property``, so it takes the generic
    ``tp_descr_get``/``tp_descr_set`` path rather than the property fast path."""

    def __init__(self, value: object) -> None:
        self.value = value

    def __get__(self, obj: object, objtype: type | None = None) -> object:
        return self if obj is None else self.value

    def __set__(self, obj: object, value: object) -> None:
        self.value = value


def _build_descriptor() -> list[object]:
    namespace: dict[str, object] = {"__slots__": ()}
    namespace.update({name: DataDescriptor(i) for i, name in enumerate(NAMES)})
    cls = _new_class("DataDescriptorReceiver", namespace)
    return [cls() for _ in range(RECEIVERS)]


def _build_getset() -> list[object]:
    # datetime's year/month/day/hour are C getset descriptors on an immutable
    # static type -- a read-only descriptor shape no pure-Python class has.
    return [datetime.datetime(2024, 1, 1, hour) for hour in range(RECEIVERS)]


def _build_method() -> list[object]:
    cls = _new_class("Methods", {name: (lambda self: 1) for name in METHOD_NAMES})
    return [cls() for _ in range(RECEIVERS)]


def _build_typeattr() -> list[object]:
    cls = _new_class("TypeAttr", {name: i for i, name in enumerate(NAMES)})
    return [cls] * RECEIVERS


def _build_module() -> list[object]:
    module = types.ModuleType("attr_cache_receiver")
    for i, name in enumerate(NAMES):
        setattr(module, name, i)
    return [module] * RECEIVERS


def _build_getattr() -> list[object]:
    cls = _new_class("GetAttrHook", {"__getattr__": lambda self, name: 1})
    return [cls() for _ in range(RECEIVERS)]


def _build_getattribute() -> list[object]:
    cls = _new_class(
        "GetAttributeOverride",
        {"__getattribute__": lambda self, name: object.__getattribute__(self, name)},
    )
    objs: list[object] = [cls() for _ in range(RECEIVERS)]
    _set_names(objs)
    return objs


def _build_poly_types(num_types: int) -> list[object]:
    """``num_types`` plain classes interleaved, so a site sees them all."""
    classes = [_new_class(f"Poly{num_types}x{i}") for i in range(num_types)]
    objs: list[object] = [
        classes[i % num_types]() for i in range(max(RECEIVERS, num_types))
    ]
    _set_names(objs)
    return objs


def _build_poly_layout() -> list[object]:
    """One site over four different cache kinds, not merely four types."""
    objs: list[object] = []
    for group in zip(
        _build_inline(), _build_slots(), _build_property(), _build_classvar()
    ):
        objs.extend(group)
    return objs


def _build_poly_type() -> list[object]:
    classes = [
        _new_class(f"PolyType{i}", {name: i for name in NAMES}) for i in range(2)
    ]
    return [classes[i % len(classes)] for i in range(RECEIVERS)]


# ---------------------------------------------------------------------------
# Layout registry
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Layout:
    description: str
    build: Callable[[], list[object]]
    names: tuple[str, ...] = NAMES
    # Whether a matching -store workload is generated.  Off for layouts where a
    # store would change the layout out from under the load workload (class
    # variables, type and module attributes, __getattr__ misses) or is simply
    # not permitted (read-only C getsets, bound methods).
    store: bool = True
    # Emit `o.name()` instead of `o.name`, i.e. the method-call form of
    # LOAD_ATTR that CinderX serves from its separate LoadMethodCache.
    call: bool = False
    # Read through a module global instead of the loop variable, which is the
    # only shape that reaches the module-attribute cache.
    module: bool = False


LAYOUTS: dict[str, Layout] = {
    "inline": Layout(
        "managed dict, values still inline in the object",
        _build_inline,
    ),
    "split": Layout(
        "inline values plus a materialized shared-key dict",
        _build_split,
    ),
    "combined": Layout(
        "managed dict detached into an unshared dict",
        _build_combined,
    ),
    "exhausted": Layout(
        "shared keys full, attribute lives in a real dict",
        _build_exhausted,
    ),
    "dictoffset": Layout(
        "non-managed heap type with a fixed tp_dictoffset",
        _build_dictoffset,
    ),
    "slots": Layout(
        "__slots__ member descriptors, no dict",
        _build_slots,
    ),
    "slotsdict": Layout(
        "__slots__ members alongside a managed dict",
        _build_slots_dict,
    ),
    "varsize": Layout(
        "managed dict on a variable-sized type",
        _build_varsize,
    ),
    "afteritems": Layout(
        "__slots__ on a tuple subclass, runtime offsets (3.15+)",
        _build_after_items,
    ),
    "classvar": Layout(
        "plain class variable, no instance attribute",
        _build_classvar,
        store=False,
    ),
    "property": Layout(
        "property, i.e. a Python data descriptor",
        _build_property,
    ),
    "descriptor": Layout(
        "data descriptor that is not a property",
        _build_descriptor,
    ),
    "getset": Layout(
        "C getset descriptors on an immutable static type",
        _build_getset,
        names=("year", "month", "day", "hour"),
        store=False,
    ),
    "method": Layout(
        "bound-method form of LOAD_ATTR",
        _build_method,
        names=METHOD_NAMES,
        store=False,
        call=True,
    ),
    "typeattr": Layout(
        "attribute read off a class object",
        _build_typeattr,
        store=False,
    ),
    "module": Layout(
        "attribute read off a module global",
        _build_module,
        store=False,
        module=True,
    ),
    # Same receivers as ``module``, but reached through the untyped loop
    # variable rather than a module global.  ``module`` above is compiled with
    # the receiver statically known, so it is served by LoadModuleAttrCache and
    # never reaches the attribute cache at all; this one leaves the JIT unable
    # to prove the receiver is a module, so it goes through a cache and
    # is the only workload that exercises discovering a module at runtime.
    "dynmodule": Layout(
        "module reached through an untyped local rather than a global",
        _build_module,
        store=False,
    ),
    "getattr": Layout(
        "__getattr__ fallback for a missing attribute",
        _build_getattr,
        store=False,
    ),
    "getattribute": Layout(
        "__getattribute__ override, uncacheable",
        _build_getattribute,
        store=False,
    ),
    "poly2": Layout(
        "2 receiver types at one site",
        lambda: _build_poly_types(2),
    ),
    "poly4": Layout(
        "4 receiver types, exactly filling the cache",
        lambda: _build_poly_types(4),
    ),
    "poly8": Layout(
        "8 receiver types, megamorphic",
        lambda: _build_poly_types(8),
    ),
    "poly16": Layout(
        "16 receiver types, deeply megamorphic",
        lambda: _build_poly_types(16),
    ),
    "polylayout": Layout(
        "4 receiver layouts at one site",
        _build_poly_layout,
        store=False,
    ),
    "polytype": Layout(
        "2 class objects at one type-attribute site",
        _build_poly_type,
        store=False,
    ),
}


def workload_names() -> list[str]:
    names: list[str] = []
    for name, layout in LAYOUTS.items():
        names.append(name)
        if layout.store:
            names.append(f"{name}-store")
    return names


# ---------------------------------------------------------------------------
# Hot-function generation
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Benchmark:
    name: str
    description: str
    step: Callable[[], object]
    fn: Callable[..., object]
    receivers: list[object] = field(repr=False)
    ops_per_call: int


def _generate(
    name: str, source: str, extra_globals: dict[str, object]
) -> Callable[..., object]:
    namespace: dict[str, object] = dict(extra_globals)
    exec(compile(source, f"<attr_cache:{name}>", "exec"), namespace)
    return namespace[name]  # pyre-ignore[7]


def _build_load_fn(
    slug: str, layout: Layout, extra_globals: dict[str, object]
) -> Callable[..., object]:
    receiver = "MODULE" if layout.module else "o"
    suffix = "()" if layout.call else ""
    body = [
        f"            v = {receiver}.{name}{suffix}"
        for _ in range(UNROLL)
        for name in layout.names
    ]
    source = "\n".join(
        [
            f"def {slug}(objs):",
            "    v = None",
            "    for o in objs:",
            *body,
            "    return v",
            "",
        ]
    )
    return _generate(slug, source, extra_globals)


def _build_store_fn(slug: str, layout: Layout) -> Callable[..., object]:
    body = [f"        o.{name} = value" for _ in range(UNROLL) for name in layout.names]
    source = "\n".join(
        [
            f"def {slug}(objs, value):",
            "    for o in objs:",
            *body,
            "",
        ]
    )
    return _generate(slug, source, {})


def build_benchmark(name: str) -> Benchmark:
    """Construct the receivers and the generated hot function for a workload.

    Every workload gets a freshly generated function, so its inline caches are
    never shared with another workload's receivers.
    """
    is_store = name.endswith("-store")
    layout_name = name[: -len("-store")] if is_store else name
    layout = LAYOUTS[layout_name]

    receivers = layout.build()
    slug = f"{'store' if is_store else 'load'}_{layout_name}"

    fn: Callable[..., object]
    if is_store:
        fn = _build_store_fn(slug, layout)

        def step() -> object:
            return fn(receivers, STORE_VALUE)

    else:
        extra: dict[str, object] = {"MODULE": receivers[0]} if layout.module else {}
        fn = _build_load_fn(slug, layout, extra)

        def step() -> object:
            return fn(receivers)

    return Benchmark(
        name=name,
        description=layout.description,
        step=step,
        fn=fn,
        receivers=receivers,
        ops_per_call=len(receivers) * len(layout.names) * UNROLL,
    )


# ---------------------------------------------------------------------------
# Layout verification
# ---------------------------------------------------------------------------


def describe_receivers(receivers: list[object]) -> str:
    """A one-line summary of what a workload's receivers actually are.

    Layouts are reached by side effect -- exhausting shared keys, detaching a
    dict -- so this reports the layout that was really produced rather than the
    one the builder was aiming for.
    """
    obj = receivers[0]
    cls = obj if isinstance(obj, type) else type(obj)
    facts = [
        f"type={cls.__name__}",
        f"dictoffset={cls.__dictoffset__}",
        f"basicsize={cls.__basicsize__}",
        f"itemsize={cls.__itemsize__}",
    ]
    flags = cls.__flags__
    if flags & TPFLAGS_MANAGED_DICT:
        facts.append("MANAGED_DICT")
    if flags & TPFLAGS_INLINE_VALUES:
        facts.append("INLINE_VALUES")

    internals = _internals()
    inline_values = _probe(internals, "has_inline_values", obj)
    if inline_values != "n/a":
        facts.append(f"inline_values={inline_values}")
    if not isinstance(obj, type):
        facts.append(f"dict={_dict_state(obj, internals)}")

    types_seen = {type(receiver).__name__ for receiver in receivers}
    if len(types_seen) > 1:
        facts.append(f"receiver_types={len(types_seen)}")
    return " ".join(facts)


def describe_workload(name: str) -> str:
    try:
        return describe_receivers(build_benchmark(name).receivers)
    except Unsupported as exc:
        return f"unsupported: {exc}"


def _dict_state(obj: object, internals: object | None) -> str:
    """Whether the receiver already has a ``__dict__``, and of what kind.

    Read through the GC rather than the attribute, because merely touching
    ``obj.__dict__`` is itself one of the things that changes the layout.
    """
    instance_dict = None
    for referent in gc.get_referents(obj):
        if type(referent) is dict:
            instance_dict = referent
            break
    if instance_dict is None:
        return "absent"
    if internals is None:
        return "present"
    return {"True": "split", "False": "combined", "n/a": "present"}[
        _probe(internals, "has_split_table", instance_dict)
    ]


def _internals() -> object | None:
    """``_testinternalcapi`` exposes the layout predicates on 3.14+ only.

    Imported by name because it is a CPython test module with no stubs and no
    guarantee of being present.
    """
    try:
        return importlib.import_module("_testinternalcapi")
    except ImportError:
        return None


def _probe(internals: object | None, predicate: str, obj: object) -> str:
    probe = getattr(internals, predicate, None)
    if probe is None:
        return "n/a"
    try:
        return str(bool(probe(obj)))
    except (TypeError, ValueError):
        return "n/a"


def specialization_summary(fn: Callable[..., object]) -> str:
    """Which adaptive-interpreter specializations CPython settled on."""
    counts: dict[str, int] = {}
    try:
        instructions = list(dis.get_instructions(fn, adaptive=True))
    except (TypeError, ValueError):
        return "unavailable"
    for instruction in instructions:
        opname = instruction.opname
        if opname.startswith(("LOAD_ATTR", "STORE_ATTR")):
            counts[opname] = counts.get(opname, 0) + 1
    if not counts:
        return "none"
    return " ".join(f"{opname}={count}" for opname, count in sorted(counts.items()))


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------


def run_iterations(step: Callable[[], object], iterations: int) -> float:
    start = time.perf_counter()
    for _ in range(iterations):
        step()
    return time.perf_counter() - start


def run(benchmark: Benchmark, iterations: int, warmup: int, repeat: int) -> list[float]:
    print(f"Warmup ({warmup} iterations)...", file=sys.stderr)
    for _ in range(warmup):
        benchmark.step()

    # A background compile landing mid-run shows up as a single wild sample, and
    # so does a collection of everything the setup phase allocated.
    cinderx.jit.wait_for_background_compiles()
    gc.collect()

    print(f"Timed runs ({repeat} x {iterations} iterations)...", file=sys.stderr)
    samples_ns: list[float] = []
    for i in range(repeat):
        elapsed = run_iterations(benchmark.step, iterations)
        per_op_ns = elapsed / (iterations * benchmark.ops_per_call) * 1e9
        samples_ns.append(per_op_ns)
        print(f"  Run {i + 1}/{repeat}: {per_op_ns:.2f} ns/op", file=sys.stderr)
    return samples_ns


def build_subprocess_env(extra: dict[str, str]) -> dict[str, str]:
    env = {key: os.environ[key] for key in SUBPROCESS_ENV_KEYS if key in os.environ}
    env.update(extra)
    return env


def reexec_prefix() -> list[str]:
    """Command prefix that re-runs this benchmark in a fresh process.

    When packaged (a PAR/XAR from ``buck run``/``buck build``) ``__file__`` lives
    under an extracted mount and only the binary itself has the bundled deps on
    ``sys.path``, so re-exec the binary.  A plain ``python script.py`` invocation
    re-execs the interpreter with the script.
    """
    if "/xarfuse/" in os.path.abspath(__file__) or sys.argv[0].endswith(
        (".par", ".xar")
    ):
        return [sys.argv[0]]
    return [sys.executable, os.path.abspath(__file__)]


def select_workloads(workload: str | None) -> list[str]:
    """A single named workload, or all of them (registry order) when unspecified."""
    return [workload] if workload else workload_names()


def run_compare(argv: list[str]) -> None:
    """Re-exec this benchmark twice (interpreter baseline vs JIT) and print, per
    workload, the speedup -- plus the geomean across workloads."""
    forwarded = [a for a in argv if a not in ("--compare", "--cinderx")]
    prefix = reexec_prefix()

    def measure(
        label: str, env_extra: dict[str, str], extra_args: list[str]
    ) -> dict[str, float]:
        env = build_subprocess_env(env_extra)
        cmd = [*prefix, *forwarded, *extra_args]
        print(f"\n--- {label} ---")
        result = subprocess.run(cmd, env=env, capture_output=True, text=True)
        sys.stdout.write(result.stderr)
        sys.stdout.write(result.stdout)
        if result.returncode:
            print(f"Error: {label} run failed", file=sys.stderr)
            sys.exit(result.returncode)

        means: dict[str, float] = {}
        for line in result.stderr.splitlines():
            line = line.strip()
            if line.startswith("result:"):
                _, name, mean = line.split()
                means[name] = float(mean)
        if not means:
            print(f"Error: could not parse results from {label} run", file=sys.stderr)
            sys.exit(1)
        return means

    baseline = measure("Baseline (CINDERX_DISABLE=1)", {"CINDERX_DISABLE": "1"}, [])
    jit = measure("CinderX JIT", {}, ["--cinderx"])

    names = [n for n in workload_names() if n in baseline and n in jit]
    print(f"\n{'=' * 60}")
    print(f"{'workload':<18}{'baseline':>13}{'jit':>13}{'speedup':>12}")
    print("-" * 60)
    ratios: list[float] = []
    for name in names:
        base_ns = baseline[name]
        jit_ns = jit[name]
        speedup = base_ns / jit_ns if jit_ns else float("nan")
        ratios.append(speedup)
        print(f"{name:<18}{base_ns:>11.2f}ns{jit_ns:>11.2f}ns{speedup:>11.2f}x")
    print("-" * 60)
    if ratios:
        geomean = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
        print(f"{'geomean':<18}{'':>13}{'':>13}{geomean:>11.2f}x")
    print(f"{'=' * 60}  (ns per attribute op; higher speedup is better)")


def print_results(
    benchmark: Benchmark,
    warmup: int,
    iterations: int,
    repeat: int,
    enable_cinderx: bool,
    samples_ns: list[float],
    show_specialization: bool,
) -> float:
    mean_ns = sum(samples_ns) / len(samples_ns)
    median_ns = statistics.median(samples_ns)

    print("", file=sys.stderr)
    print("=" * 60, file=sys.stderr)
    print(f"workload={benchmark.name}", file=sys.stderr)
    print(f"  layout: {describe_receivers(benchmark.receivers)}", file=sys.stderr)
    print(
        f"  warmup={warmup} iterations_per_run={iterations} runs={repeat} "
        f"ops_per_iteration={benchmark.ops_per_call}",
        file=sys.stderr,
    )
    print(
        f"  run times: {[f'{sample:.2f}ns' for sample in samples_ns]}", file=sys.stderr
    )
    print(f"  mean: {mean_ns:.2f} ns/op", file=sys.stderr)
    print(f"  median: {median_ns:.2f} ns/op (reported)", file=sys.stderr)
    print(
        f"  JIT requested={'yes' if enable_cinderx else 'no'} "
        f"compiled={cinderx.jit.is_jit_compiled(benchmark.fn)}",
        file=sys.stderr,
    )
    if show_specialization:
        print(f"  specialized: {specialization_summary(benchmark.fn)}", file=sys.stderr)
    print("=" * 60, file=sys.stderr)
    # Machine-parseable line consumed by --compare (one per workload).  The
    # median, not the mean: at a few nanoseconds an op a single scheduling hiccup
    # is a 10x sample.
    print(f"result: {benchmark.name} {median_ns:.4f}", file=sys.stderr)
    return median_ns


def print_describe(names: list[str]) -> None:
    print(f"Python {sys.version.split()[0]}")
    for name in names:
        print(f"{name:<20}{describe_workload(name)}")


@click.command(context_settings={"help_option_names": ["-h", "--help"]})
@click.option(
    "--cinderx", "enable_cinderx", is_flag=True, help="Enable the CinderX JIT"
)
@click.option(
    "--workload",
    type=click.Choice(workload_names()),
    default=None,
    help="Run a single attribute workload; if omitted, run all of them",
)
@click.option(
    "--iterations",
    type=click.IntRange(min=1),
    default=1000,
    show_default=True,
    help="Number of timed iterations per run",
)
@click.option(
    "--warmup",
    type=click.IntRange(min=0),
    default=1000,
    show_default=True,
    help="Number of warmup iterations before timing",
)
@click.option(
    "--repeat",
    type=click.IntRange(min=1),
    default=10,
    show_default=True,
    help="Number of timed runs; the median of these is what gets reported",
)
@click.option(
    "--compile-after-n-calls",
    type=click.IntRange(min=0),
    default=None,
    help="Override the JIT call-count threshold when --cinderx is enabled",
)
@click.option(
    "--compare",
    is_flag=True,
    help="Re-exec in two subprocesses (baseline vs JIT) and print the speedup",
)
@click.option(
    "--describe",
    is_flag=True,
    help="Print the layout each workload's receivers actually have, then exit",
)
@click.option(
    "--specialization",
    is_flag=True,
    help="Report which adaptive-interpreter specializations each site settled on",
)
def cli(
    enable_cinderx: bool,
    workload: str | None,
    iterations: int,
    warmup: int,
    repeat: int,
    compile_after_n_calls: int | None,
    compare: bool,
    describe: bool,
    specialization: bool,
) -> None:
    names = select_workloads(workload)

    if describe:
        print_describe(names)
        return

    if compare:
        run_compare(sys.argv[1:])
        return

    print(f"Python {sys.version.split()[0]}", file=sys.stderr)
    print(
        f"CinderX attribute-cache benchmark ({len(names)} workload(s)) "
        f"warmup={warmup} iterations={iterations} repeat={repeat}",
        file=sys.stderr,
    )

    if enable_cinderx:
        cinderx.jit.auto()
        if compile_after_n_calls is not None:
            cinderx.jit.compile_after_n_calls(compile_after_n_calls)
        if cinderx.jit.is_enabled():
            print("Enabled the CinderX JIT", file=sys.stderr)
        else:
            # The _cinderx native extension is only wired into python_binary for
            # some Python versions; on the rest cinderx.jit is an inert stub and
            # every workload measures the interpreter twice.
            print(
                "WARNING: the CinderX JIT is unavailable on this runtime, "
                "measuring the interpreter instead",
                file=sys.stderr,
            )

    means: dict[str, float] = {}
    for name in names:
        try:
            benchmark = build_benchmark(name)
        except Unsupported as exc:
            print(f"\nSkipping workload {name}: {exc}", file=sys.stderr)
            continue
        print(
            f"\nSetting up workload {name} ({benchmark.description})...",
            file=sys.stderr,
        )
        if enable_cinderx:
            # Compile the hot function up front rather than waiting for the call
            # threshold, which differs by Python version.  Anything it calls --
            # property getters, __getattr__ hooks -- still comes in via auto().
            cinderx.jit.force_compile(benchmark.fn)
        samples_ns = run(benchmark, iterations, warmup, repeat)
        means[name] = print_results(
            benchmark,
            warmup,
            iterations,
            repeat,
            enable_cinderx,
            samples_ns,
            specialization,
        )

    if len(means) > 1:
        print(f"\n{'=' * 40}", file=sys.stderr)
        print("Summary (median ns/attribute op)", file=sys.stderr)
        for name, mean_ns in means.items():
            print(f"  {name:<22}{mean_ns:>10.2f}", file=sys.stderr)
        print(f"{'=' * 40}", file=sys.stderr)


if __name__ == "__main__":
    cli()
