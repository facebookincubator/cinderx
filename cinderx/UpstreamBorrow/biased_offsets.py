# Copyright (c) Meta Platforms, Inc. and affiliates.

"""Rewrite direct interpreter/thread-state field accesses to runtime-biased
pointers (see cinderx/Common/runtime_offsets.h).

CinderX can load into a runtime built from different headers, so offsets
compiled into borrowed or generated code read garbage when the loaded
runtime's layout differs. The CiInterpPre()/CiInterpPost()/
CiInterpMeta()/CiTstatePost() macros correct the base pointer from
_PyRuntime.debug_offsets and are no-ops when layouts match.

Used by UpstreamBorrow.py for borrowed code and by the regen-cases-*.sh
scripts for generated interpreter cases. Kept free of third-party imports so
it runs on plain fbpython.
"""

import re
import sys

# Field names by interpreter/thread-state region, see
# cinderx/Common/runtime_offsets.h. Receivers are matched explicitly so that
# e.g. interp->brc (region C) and tstate->brc (region D) rewrite differently.
# Every pattern requires the receiver immediately before `->`, which also
# makes the rewrite idempotent (biased output never matches again).
# Parenthesized receivers such as `(interp)->types` (as in the borrowed
# NEXT_VERSION_TAG macro) get their own patterns: folding the parens into the
# receiver would also match the `(` of previously biased output.
_RECEIVER_START = r"(?<![\w>])"
_PAREN_RECEIVER_START = r"(?<![\w>])\("
_PAREN_RECEIVER_END = r"\)"
_INTERP_RECEIVERS = (
    r"(?:interp|tstate->interp|_PyInterpreterState_GET\(\)|"
    r"PyInterpreterState_Get\(\)|i)"
)
_TSTATE_RECEIVERS = r"(?:tstate|tstate_impl)"
_INTERP_PRE_FIELDS = (
    "sysdict|builtins|imports|_gil|_code_object_generation|codecs|config|"
    "dict|eval_frame|func_watchers|active_func_watchers|co_extra_user_count|"
    "co_extra_freefuncs|xi|before_forkers|warnings|atexit|stoptheworld|qsbr"
)
_INTERP_POST_FIELDS = (
    "brc|unique_ids|weakref_locks|tlbc_indices|asyncio_tasks_head|"
    "asyncio_tasks_lock|obmalloc|audit_hooks|type_watchers|code_watchers|"
    "context_watchers|active_code_watchers|active_context_watchers|"
    "object_state|unicode|long_state|dtoa|func_state|code_state|dict_state|"
    "exc_state|mem_free_queue|ast|types|callable_cache|common_consts|"
    "trace_run_counter|rare_events|builtins_dict_watcher|monitors|"
    "sys_profiling_threads|sys_tracing_threads|monitoring_callables|"
    "monitoring_tool_names|monitoring_tool_versions|cached_objects|"
    "static_objects"
)
_INTERP_META_FIELDS = (
    "lazy_imports|lazy_import_verbose_seen|excluding_modules|eager_imports|"
    "lazy_modules|lazy_imports_mutex|jit_executable_type"
)
_TSTATE_POST_FIELDS = (
    "freelists|brc|refcounts|tlbc_index|reftotal|"
    "suppress_co_const_immortalization|c_stack_init_base|"
    "c_stack_init_top"
)

_BIASED_PATTERNS: list[tuple[re.Pattern[str], str]] = [
    (
        re.compile(
            rf"{_RECEIVER_START}(?P<recv>{_INTERP_RECEIVERS})->(?P<field>{_INTERP_PRE_FIELDS})\b"
        ),
        r"CiInterpPre(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            rf"{_PAREN_RECEIVER_START}(?P<recv>{_INTERP_RECEIVERS}){_PAREN_RECEIVER_END}->(?P<field>{_INTERP_PRE_FIELDS})\b"
        ),
        r"CiInterpPre(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            rf"{_RECEIVER_START}(?P<recv>{_INTERP_RECEIVERS})->(?P<field>{_INTERP_POST_FIELDS})\b"
        ),
        r"CiInterpPost(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            rf"{_PAREN_RECEIVER_START}(?P<recv>{_INTERP_RECEIVERS}){_PAREN_RECEIVER_END}->(?P<field>{_INTERP_POST_FIELDS})\b"
        ),
        r"CiInterpPost(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            rf"{_RECEIVER_START}(?P<recv>{_INTERP_RECEIVERS})->(?P<field>{_INTERP_META_FIELDS})\b"
        ),
        r"CiInterpMeta(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            rf"{_PAREN_RECEIVER_START}(?P<recv>{_INTERP_RECEIVERS}){_PAREN_RECEIVER_END}->(?P<field>{_INTERP_META_FIELDS})\b"
        ),
        r"CiInterpMeta(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            r"\(runtime\)->_main_interpreter\.(?P<field>" + _INTERP_POST_FIELDS + r")\b"
        ),
        r"CiInterpPost(Ci_MainInterpreter(runtime))->\g<field>",
    ),
    (
        re.compile(
            r"\(runtime\)->_main_interpreter\.(?P<field>" + _INTERP_META_FIELDS + r")\b"
        ),
        r"CiInterpMeta(Ci_MainInterpreter(runtime))->\g<field>",
    ),
    (
        re.compile(
            rf"{_RECEIVER_START}(?P<recv>{_TSTATE_RECEIVERS})->(?P<field>{_TSTATE_POST_FIELDS})\b"
        ),
        r"CiTstatePost(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            rf"{_PAREN_RECEIVER_START}(?P<recv>{_TSTATE_RECEIVERS}){_PAREN_RECEIVER_END}->(?P<field>{_TSTATE_POST_FIELDS})\b"
        ),
        r"CiTstatePost(\g<recv>)->\g<field>",
    ),
    (
        re.compile(
            r"\(\(\s*_PyThreadStateImpl\s*\*\s*\)\s*"
            rf"(?P<recv>{_TSTATE_RECEIVERS})\s*\)->(?P<field>{_TSTATE_POST_FIELDS})\b"
        ),
        r"CiTstatePost(\g<recv>)->\g<field>",
    ),
]


def transform_biased_offsets(lines: list[str]) -> list[str]:
    """Rewrite direct interpreter/thread-state field accesses in borrowed
    or generated code to runtime-corrected biased pointers."""
    out = []
    for line in lines:
        for pattern, replacement in _BIASED_PATTERNS:
            line = pattern.sub(replacement, line)
        out.append(line)
    return out


def main() -> None:
    for path in sys.argv[1:]:
        with open(path) as f:
            lines = f.read().splitlines()
        with open(path, "w") as f:
            f.write("\n".join(transform_biased_offsets(lines)))
            f.write("\n")


if __name__ == "__main__":
    main()
