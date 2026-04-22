# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-ignore-all-errors

# Vendored version of https://github.com/colesbury/fastmark with modifications
# to work with cinderx

import argparse
import inspect
import io
import os
import sys
import time
import types

EXCLUDED = {
    "asyncio_tcp",
    "asyncio_tcp_ssl",
    "bench_mp_pool",  # uses subprocess
    "bench_thread_pool",  # for now
}

if sys.version_info >= (3, 13):
    # SQLAlchemy 1.4 is not available for 3.13+ internally
    EXCLUDED.add("sqlalchemy_declarative")
    EXCLUDED.add("sqlalchemy_imperative")


def ADD_PATH(path):
    sys.path.append(os.path.join(os.path.dirname(__file__), path))


PYPERFORMANCE = os.path.join(os.path.dirname(__file__), "pyperformance")

if not os.path.exists(PYPERFORMANCE):
    print(
        f"Error: pyperformance directory not found at {PYPERFORMANCE}", file=sys.stderr
    )
    sys.exit(1)

BENCHMARKS = os.path.join(PYPERFORMANCE, "data-files", "benchmarks")

ADD_PATH(os.path.join(BENCHMARKS, "bm_regex_compile"))
ADD_PATH(BENCHMARKS)
ADD_PATH(os.path.join(BENCHMARKS, "bm_2to3", "vendor", "src"))

# Disable some logging
import logging

logging.getLogger("RefactoringTool").setLevel(level=logging.ERROR)
logging.getLogger("websockets.server").setLevel(level=logging.ERROR)


def run_2to3(loops):
    import glob
    import lib2to3.main

    datadir = os.path.join(BENCHMARKS, "bm_2to3", "data", "2to3")
    pyfiles = glob.glob(os.path.join(datadir, "*.py.txt"))

    # Redirect stdout to capture the output
    stdout = sys.stdout
    sys.stdout = io.StringIO()
    start = time.perf_counter()
    for _ in range(loops):
        lib2to3.main.main("lib2to3.fixes", ["-f", "all", *pyfiles])
    end = time.perf_counter()
    sys.stdout = stdout

    return end - start


def run_bpe_tokeniser(loops):
    # Shorten the data file to speed up the benchmark
    import bm_bpe_tokeniser.run_benchmark as bm_bpe_tokeniser

    DATA = os.path.join(
        BENCHMARKS, "bm_bpe_tokeniser", "data", "frankenstein_intro.txt"
    )
    with open(DATA, "r", encoding="utf8") as f:
        data = f.read()

    # Shorten the data to 3000 characters to speed up the benchmark
    data = data[:3000] + "\n"

    start = time.perf_counter()
    for _ in range(loops):
        bm_bpe_tokeniser.train(data)
    end = time.perf_counter()
    return end - start


def run_chaos(loops):
    import bm_chaos.run_benchmark as bm_chaos
    from benchmarks.bm_chaos.run_benchmark import Chaosgame, GVector, Spline

    splines = [
        Spline(
            [
                GVector(1.597350, 3.304460, 0.000000),
                GVector(1.575810, 4.123260, 0.000000),
                GVector(1.313210, 5.288350, 0.000000),
                GVector(1.618900, 5.329910, 0.000000),
                GVector(2.889940, 5.502700, 0.000000),
                GVector(2.373060, 4.381830, 0.000000),
                GVector(1.662000, 4.360280, 0.000000),
            ],
            3,
            [0, 0, 0, 1, 1, 1, 2, 2, 2],
        ),
        Spline(
            [
                GVector(2.804500, 4.017350, 0.000000),
                GVector(2.550500, 3.525230, 0.000000),
                GVector(1.979010, 2.620360, 0.000000),
                GVector(1.979010, 2.620360, 0.000000),
            ],
            3,
            [0, 0, 0, 1, 1, 1],
        ),
        Spline(
            [
                GVector(2.001670, 4.011320, 0.000000),
                GVector(2.335040, 3.312830, 0.000000),
                GVector(2.366800, 3.233460, 0.000000),
                GVector(2.366800, 3.233460, 0.000000),
            ],
            3,
            [0, 0, 0, 1, 1, 1],
        ),
    ]

    chaos = Chaosgame(splines, bm_chaos.DEFAULT_THICKNESS)
    start = time.perf_counter()
    for _ in range(loops):
        chaos.create_image_chaos(
            bm_chaos.DEFAULT_WIDTH,
            bm_chaos.DEFAULT_HEIGHT,
            bm_chaos.DEFAULT_ITERATIONS,
            None,
            bm_chaos.DEFAULT_RNG_SEED,
        )
    end = time.perf_counter()
    return end - start


def bench_sympy(loops, func_name):
    import bm_sympy.run_benchmark as bm_sympy
    from sympy.core.cache import clear_cache

    dt = 0

    func = getattr(bm_sympy, func_name)

    for _ in range(loops):
        # Don't benchmark clear_cache(), exclude it of the benchmark
        clear_cache()

        t0 = time.perf_counter()
        func()
        dt += time.perf_counter() - t0

    return dt


def bench_tomli_loads(loops):
    # Shrink data file to speed up the benchmark
    import tomli

    DATA_FILE = os.path.join(
        BENCHMARKS, "bm_tomli_loads", "data", "tomli-bench-data.toml"
    )
    with open(DATA_FILE, "r", encoding="utf-8") as f:
        data = f.read(1682421)

    t0 = time.perf_counter()
    for _ in range(loops):
        tomli.loads(data)
    return time.perf_counter() - t0


def bench_mako(loops):
    import mako  # noqa
    from mako.lookup import TemplateLookup  # noqa
    from mako.template import Template  # noqa
    import bm_mako.run_benchmark as bm_mako  # noqa  # isort: skip

    table_size = 150
    nparagraph = 50
    img_count = 50

    lookup = TemplateLookup()
    lookup.put_string("base.mako", bm_mako.BASE_TEMPLATE)
    lookup.put_string("page.mako", bm_mako.PAGE_TEMPLATE)

    template = Template(bm_mako.CONTENT_TEMPLATE, lookup=lookup)

    table = [range(table_size) for i in range(table_size)]
    paragraphs = range(nparagraph)
    title = "Hello world!"

    start = time.perf_counter()
    for _ in range(loops):
        template.render(
            table=table,
            paragraphs=paragraphs,
            lorem=bm_mako.LOREM_IPSUM,
            title=title,
            img_count=img_count,
            range=range,
        )
    return time.perf_counter() - start


def run_mdp(loops):
    import bm_mdp.run_benchmark as bm_mdp

    # tolerance = 0.192
    tolerance = 0.2  # decreased to speed up the benchmark

    start = time.perf_counter()
    for _ in range(loops):
        bm_mdp.Battle().evaluate(tolerance)
    dt = time.perf_counter() - start

    return dt


def bench_pprint_pformat(loops):
    from pprint import PrettyPrinter

    # Note: reduced size by 10x to speed up the benchmark
    printable = [("string", (1, 2), [3, 4], {5: 6, 7: 8})] * 10_000
    p = PrettyPrinter()
    start = time.perf_counter()
    for _ in range(loops):
        p.pformat(printable)
    return time.perf_counter() - start


def bench_pprint_safe_repr(loops):
    from pprint import PrettyPrinter

    # Note: reduced size by 10x to speed up the benchmark
    printable = [("string", (1, 2), [3, 4], {5: 6, 7: 8})] * 10_000
    p = PrettyPrinter()
    start = time.perf_counter()
    for _ in range(loops):
        p._safe_repr(printable, {}, None, 0)
    return time.perf_counter() - start


def bench_richards(loops):
    import bm_richards.run_benchmark as bm_richards

    richard = bm_richards.Richards()
    start = time.perf_counter()
    richard.run(loops)
    end = time.perf_counter()
    return end - start


def bench_richards_super(loops):
    import bm_richards_super.run_benchmark as bm_richards_super

    richard = bm_richards_super.Richards()
    start = time.perf_counter()
    richard.run(loops)
    end = time.perf_counter()
    return end - start


def bench_docutils(loops):
    import contextlib

    import docutils
    from docutils import core

    # from pathlib import Path
    # DOC_ROOT = (Path(__file__).parent / "pyperformance" / "benchmarks" / "bm_docutils" / "data" / "docs").resolve()
    # filenames = list(DOC_ROOT.rglob("*.txt"))

    # three randomly chosen files to speed up the benchmark
    filenames = [
        "user/tools.txt",
        "peps/pep-0257.txt",
        "user/rst/quickstart.txt",
    ]

    file_contents = []
    for filename in filenames:
        path = os.path.join(BENCHMARKS, "bm_docutils", "data", "docs", filename)
        with open(path, "r", encoding="utf-8") as f:
            file_contents.append(f.read())

    start = time.perf_counter()
    for _ in range(loops):
        for text in file_contents:
            with contextlib.suppress(docutils.ApplicationError):
                core.publish_string(
                    source=text,
                    reader_name="standalone",
                    parser_name="restructuredtext",
                    writer_name="html5",
                    settings_overrides={
                        "input_encoding": "unicode",
                        "output_encoding": "unicode",
                        "report_level": 5,
                    },
                )
    return time.perf_counter() - start


def dulwich_get_repo():
    import benchmarks.bm_dulwich_log.run_benchmark as bm_dulwich_log
    import dulwich.repo

    repo_path = os.path.join(BENCHMARKS, "bm_dulwich_log", "data", "asyncio.git")
    repo = dulwich.repo.Repo(repo_path)
    head = repo.head()
    bm_dulwich_log.head = head  # oof
    return repo


def genshi_text_args():
    import bm_genshi.run_benchmark as bm_genshi

    return bm_genshi.NewTextTemplate, bm_genshi.BIGTABLE_TEXT


def genshi_xml_args():
    import bm_genshi.run_benchmark as bm_genshi

    return bm_genshi.MarkupTemplate, bm_genshi.BIGTABLE_XML


def html5lib_args():
    filename = os.path.join(BENCHMARKS, "bm_html5lib", "data", "w3_tr_html5.html")
    with open(filename, "rb") as fp:
        return io.BytesIO(fp.read())


def json_dumps_args():
    import bm_json_dumps.run_benchmark as bm_json_dumps

    data = []
    for case in bm_json_dumps.CASES:
        obj, count = getattr(bm_json_dumps, case)
        data.append((obj, range(count)))
    return data


def json_loads_args():
    import json

    import bm_json_loads.run_benchmark as bm_json_loads

    json_dict = json.dumps(bm_json_loads.DICT)
    json_tuple = json.dumps(bm_json_loads.TUPLE)
    json_dict_group = json.dumps(bm_json_loads.DICT_GROUP)
    objs = (json_dict, json_tuple, json_dict_group)
    return (objs,)


def logging_args():
    stream = io.StringIO()

    import logging

    handler = logging.StreamHandler(stream=stream)
    logger = logging.getLogger("benchlogger")
    logger.propagate = False
    logger.addHandler(handler)
    logger.setLevel(logging.WARNING)
    return logger, stream


def meteor_contest_args():
    from benchmarks.bm_meteor_contest.run_benchmark import (
        get_footprints,
        get_puzzle,
        get_senh,
        HEIGHT,
        SOLVE_ARG,
        WIDTH,
    )

    board, cti, pieces = get_puzzle(WIDTH, HEIGHT)
    fps = get_footprints(board, cti, pieces)
    se_nh = get_senh(board, cti)
    return board, pieces, SOLVE_ARG, fps, se_nh


def pathlib_args():
    import shutil

    import bm_pathlib.run_benchmark as bm_pathlib

    tmp_path = bm_pathlib.setup(bm_pathlib.NUM_FILES)

    class DeletePathOnExit(str):
        def __del__(self):
            shutil.rmtree(self)

    return DeletePathOnExit(tmp_path)


def pickle_args():
    import pickle

    options = argparse.Namespace()
    options.protocol = pickle.HIGHEST_PROTOCOL

    # Use Python versions
    pickle_mod = types.ModuleType("pickle")
    pickle_mod.dumps = pickle._dumps
    pickle_mod.loads = pickle._loads

    return pickle_mod, options


def etree_args(func_name):
    def get_args():
        import xml.etree.ElementTree as etree

        import bm_xml_etree.run_benchmark as bm_xml_etree

        return etree, getattr(bm_xml_etree, f"bench_{func_name}")

    return get_args


def pyflate_args():
    filename = os.path.join(BENCHMARKS, "bm_pyflate", "data", "interpreter.tar.bz2")
    return filename


def regex_compile_args():
    import bm_regex_compile.run_benchmark as bm_regex_compile

    return bm_regex_compile.capture_regexes()


def regex_dna_args():
    import bm_regex_dna.run_benchmark as bm_regex_dna

    expected_res = ([6, 26, 86, 58, 113, 31, 31, 32, 43], 1016745, 1000000, 1336326)
    seq = bm_regex_dna.init_benchmarks(
        bm_regex_dna.DEFAULT_INIT_LEN, bm_regex_dna.DEFAULT_RNG_SEED
    )
    return seq, expected_res


def scimark_sor_args():
    import bm_scimark.run_benchmark as bm_scimark

    return 100, 10, bm_scimark.Array2D


def async_tree(benchmark_name, use_task_groups=False):
    def get_func():
        import bm_async_tree.run_benchmark as bm_async_tree

        async_tree_class = bm_async_tree.BENCHMARKS[benchmark_name]
        async_tree = async_tree_class(use_task_groups=use_task_groups)
        return async_tree.run

    return get_func


def asyncio_tcp(use_ssl):
    import functools

    def get_func():
        import bm_asyncio_tcp.run_benchmark as bm_asyncio_tcp

        bm_asyncio_tcp.CHUNK_SIZE = 1024**2 * 2  # 10x smaller
        return functools.partial(bm_asyncio_tcp.main, use_ssl)

    return get_func


ALL_BENCHMARKS = {
    "2to3": ("bm_2to3", "run_2to3", "custom", 110),
    "async_generators": ("bm_async_generators", "bench_async_generators", "async", 54),
    "async_tree_cpu_io_mixed": (
        "bm_async_tree",
        async_tree("cpu_io_mixed"),
        "async",
        41,
    ),
    "async_tree_cpu_io_mixed_tg": (
        "bm_async_tree",
        async_tree("cpu_io_mixed", True),
        "async",
        41,
    ),
    "async_tree_io": ("bm_async_tree", async_tree("io"), "async", 30),
    "async_tree_io_tg": ("bm_async_tree", async_tree("io", True), "async", 32),
    "async_tree_memoization": ("bm_async_tree", async_tree("memoization"), "async", 65),
    "async_tree_memoization_tg": (
        "bm_async_tree",
        async_tree("memoization", True),
        "async",
        64,
    ),
    "async_tree_none": ("bm_async_tree", async_tree("none"), "async", 82),
    "async_tree_none_tg": ("bm_async_tree", async_tree("none", True), "async", 81),
    "asyncio_websockets": ("bm_asyncio_websockets", "main", "async", 37),
    "asyncio_tcp": ("bm_asyncio_tcp", asyncio_tcp(use_ssl=False), "async", 250),
    "asyncio_tcp_ssl": ("bm_asyncio_tcp", asyncio_tcp(use_ssl=True), "async", 65),
    "bench_mp_pool": ("bm_concurrent_imap", "bench_mp_pool", "func", 2, (2, 1000, 10)),
    "bench_thread_pool": (
        "bm_concurrent_imap",
        "bench_thread_pool",
        "func",
        64,
        (2, 1000, 10),
    ),
    "bpe_tokeniser": ("bm_bpe_tokeniser", "run_bpe_tokeniser", "custom", 57),
    "chaos": ("bm_chaos", "run_chaos", "custom", 380),
    "comprehensions": (
        "bm_comprehensions",
        "bench_comprehensions",
        "time_func",
        1300000,
    ),
    # "connected_components": ("bm_networkx",),
    "coroutines": ("bm_coroutines", "bench_coroutines", "time_func", 920),
    "coverage": ("bm_coverage", "bench_coverage", "time_func", 200),
    "create_gc_cycles": (
        "bm_gc_collect",
        "benchamark_collection",
        "time_func",
        4100,
        (100, 20),
    ),
    "crypto_pyaes": ("bm_crypto_pyaes", "bench_pyaes", "time_func", 280),
    "deepcopy": ("bm_deepcopy", "benchmark", "time_func", 83000),
    "deepcopy_memo": ("bm_deepcopy", "benchmark_memo", "time_func", 710000),
    "deepcopy_reduce": ("bm_deepcopy", "benchmark_reduce", "time_func", 8100000),
    "deltablue": ("bm_deltablue", "delta_blue", "func", 6800, (100,)),
    "django_template": ("bm_django", "?", "pyston", -180),  # pyston
    "docutils": ("bm_docutils", "bench_docutils", "custom", 200),
    "dulwich_log": (
        "bm_dulwich_log",
        "iter_all_commits",
        "func",
        370,
        dulwich_get_repo,
    ),
    "fannkuch": ("bm_fannkuch", "fannkuch", "func", 50, (9,)),
    "float": ("bm_float", "benchmark", "func", 290, (100000,)),
    "gc_traversal": (
        "bm_gc_traversal",
        "benchamark_collection",
        "time_func",
        1700,
        (1000,),
    ),
    "generators": ("bm_generators", "bench_generators", "time_func", 730),
    "genshi_text": ("bm_genshi", "bench_genshi", "time_func", 1000, genshi_text_args),
    "genshi_xml": ("bm_genshi", "bench_genshi", "time_func", 430, genshi_xml_args),
    "go": ("bm_go", "versus_cpu", "func", 145),
    "hexiom": ("bm_hexiom", "main", "time_func", 3500, (25,)),
    "html5lib": ("bm_html5lib", "bench_html5lib", "func", 340, html5lib_args),
    "json": ("bm_json", "?", "pyston", -1),  # pyston
    "json_dumps": ("bm_json_dumps", "bench_json_dumps", "func", 1600, json_dumps_args),
    "json_loads": ("bm_json_loads", "bench_json_loads", "func", 36000, json_loads_args),
    # "k_core": ("bm_networkx",),
    "logging_format": (
        "bm_logging",
        "bench_formatted_output",
        "time_func",
        300000,
        logging_args,
    ),
    "logging_silent": (
        "bm_logging",
        "bench_silent",
        "time_func",
        25000000,
        logging_args,
    ),
    "logging_simple": (
        "bm_logging",
        "bench_simple_output",
        "time_func",
        230000,
        logging_args,
    ),
    "mako": ("bm_mako", "bench_mako", "custom", 1800),
    "many_optionals": ("bm_argparse", "bm_many_optionals", "func", 22000),
    "mdp": ("bm_mdp", "run_mdp", "custom", 24),
    "meteor_contest": (
        "bm_meteor_contest",
        "bench_meteor_contest",
        "time_func",
        190,
        meteor_contest_args,
    ),
    "nbody": ("bm_nbody", "bench_nbody", "time_func", 240, ("sun", 20000)),
    "nqueens": ("bm_nqueens", "bench_n_queens", "func", 270, (8,)),
    "pathlib": ("bm_pathlib", "bench_pathlib", "time_func", 1200, pathlib_args),
    "pickle_pure_python": ("bm_pickle", "bench_pickle", "time_func", 3300, pickle_args),
    "pidigits": ("bm_pidigits", "calc_ndigits", "func", 100, (2000,)),
    "pprint_pformat": ("bm_pprint", "bench_pprint_pformat", "custom", 140),
    "pprint_safe_repr": ("bm_pprint", "bench_pprint_safe_repr", "custom", 290),
    "pycparser": (),  # pyston
    "pyflate": ("bm_pyflate", "bench_pyflake", "time_func", 47, pyflate_args),
    "pylint": (),  # pyston
    # "python_startup": ("bm_python_startup", "?", "command", 4),  # requires subprocess
    # "python_startup_no_site": ("bm_python_startup",),  # requires subprocess
    "raytrace": ("bm_raytrace", "bench_raytrace", "time_func", 99, (100, 99, None)),
    "regex_compile": (
        "bm_regex_compile",
        "bench_regex_compile",
        "time_func",
        170,
        regex_compile_args,
    ),
    "regex_dna": ("bm_regex_dna", "bench_regex_dna", "time_func", 100, regex_dna_args),
    "regex_effbot": ("bm_regex_effbot", "bench_regex_effbot", "time_func", 660),
    "regex_v8": ("bm_regex_v8", "bench_regex_v8", "time_func", 830),
    "richards": ("bm_richards", "bench_richards", "custom", 490),
    "richards_super": ("bm_richards_super", "bench_richards_super", "custom", 430),
    "scimark_fft": ("bm_scimark", "bench_FFT", "time_func", 69, (1024, 50)),
    "scimark_lu": ("bm_scimark", "bench_LU", "time_func", 190, (100,)),
    "scimark_monte_carlo": (
        "bm_scimark",
        "bench_MonteCarlo",
        "time_func",
        340,
        (100 * 1000,),
    ),
    "scimark_sor": ("bm_scimark", "bench_SOR", "time_func", 190, scimark_sor_args),
    "scimark_sparse_mat_mult": (
        "bm_scimark",
        "bench_SparseMatMult",
        "time_func",
        5000,
        (1000, 50 * 1000),
    ),
    # "shortest_path": ("bm_networkx",),
    "spectral_norm": ("bm_spectral_norm", "bench_spectral_norm", "time_func", 250),
    # "sphinx": ("bm_sphinx", "bench_sphinx", "custom", 1),  # messes with builtins.open
    "sqlalchemy_declarative": (
        "bm_sqlalchemy_declarative",
        "bench_sqlalchemy",
        "time_func",
        160,
        (100,),
    ),
    "sqlalchemy_imperative": (
        "bm_sqlalchemy_imperative",
        "bench_sqlalchemy",
        "time_func",
        1000,
        (100,),
    ),
    "sqlglot_normalize": ("bm_sqlglot_v2", "bench_normalize", "time_func", 160),
    "sqlglot_optimize": ("bm_sqlglot_v2", "bench_optimize", "time_func", 390),
    "sqlglot_parse": ("bm_sqlglot_v2", "bench_parse", "time_func", 16000),
    "sqlglot_transpile": ("bm_sqlglot_v2", "bench_transpile", "time_func", 13000),
    "sqlite_synth": ("bm_sqlite_synth", "bench_sqlite", "time_func", 7500000),
    "subparsers": ("bm_argparse", "bm_subparsers", "func", 1000),
    "sympy_expand": ("bm_sympy", "bench_sympy", "custom", 53, ("bench_expand",)),
    "sympy_integrate": (
        "bm_sympy",
        "bench_sympy",
        "custom",
        1300,
        ("bench_integrate",),
    ),
    "sympy_str": ("bm_sympy", "bench_sympy", "custom", 97, ("bench_str",)),
    "sympy_sum": ("bm_sympy", "bench_sympy", "custom", 140, ("bench_sum",)),
    "telco": (
        "bm_telco",
        "bench_telco",
        "time_func",
        2900,
        os.path.join(BENCHMARKS, "bm_telco", "data", "telco-bench.b"),
    ),
    "thrift": (),  # pyston
    "tomli_loads": ("bm_tomli_loads", "bench_tomli_loads", "custom", 100),
    "typing_runtime_protocols": (
        "bm_typing_runtime_protocols",
        "bench_protocols",
        "time_func",
        140000,
    ),
    "unpickle_pure_python": (
        "bm_pickle",
        "bench_unpickle",
        "time_func",
        4700,
        pickle_args,
    ),
    "xml_etree_generate": (
        "bm_xml_etree",
        "bench_etree",
        "time_func",
        220,
        etree_args("generate"),
    ),
    "xml_etree_iterparse": (
        "bm_xml_etree",
        "bench_etree",
        "time_func",
        150,
        etree_args("iterparse"),
    ),
    "xml_etree_parse": (
        "bm_xml_etree",
        "bench_etree",
        "time_func",
        110,
        etree_args("parse"),
    ),
    "xml_etree_process": (
        "bm_xml_etree",
        "bench_etree",
        "time_func",
        320,
        etree_args("process"),
    ),
}


def import_benchmark(module_name):
    import importlib

    mod = importlib.import_module(f"benchmarks.{module_name}.run_benchmark")
    return mod


async def wrap_async_func(func, loops):
    t0 = time.perf_counter()
    for _ in range(loops):
        await func()
    dt = time.perf_counter() - t0
    return dt


def run_one_benchmark(name, scale, decorate):
    info = ALL_BENCHMARKS[name]
    module_name, func_name, kind, loops, *extra = info
    if extra:
        args = extra[0]
    else:
        args = ()

    if isinstance(args, types.FunctionType):
        args = args()

    if not isinstance(args, tuple):
        args = (args,)

    loops = round(loops * scale / 10000)
    if loops < 1:
        loops = 1

    if kind == "custom":
        func = decorate(globals()[func_name])
        return func(loops, *args)
    elif kind == "func":
        mod = import_benchmark(module_name)
        func = decorate(getattr(mod, func_name))
        start = time.perf_counter()
        for _ in range(loops):
            func(*args)
        end = time.perf_counter()
        return end - start
    elif kind == "time_func":
        mod = import_benchmark(module_name)
        func = decorate(getattr(mod, func_name))
        return func(loops, *args)
    elif kind == "async":
        import asyncio

        mod = import_benchmark(module_name)
        if isinstance(func_name, str):
            func = getattr(mod, func_name)
        else:
            func = func_name()
        func = decorate(func)
        dt = asyncio.run(wrap_async_func(func, loops))
        return dt


def identity(func):
    return func


def record_stats(func):
    if inspect.iscoroutinefunction(func):

        async def record_stats_async(*args, **kwargs):
            sys._stats_on()
            try:
                return await func(*args, **kwargs)
            finally:
                sys._stats_off()

        return record_stats_async
    else:

        def record_stats_sync(*args, **kwargs):
            sys._stats_on()
            try:
                return func(*args, **kwargs)
            finally:
                sys._stats_off()

        return record_stats_sync


def main(args):
    print(f"Python {sys.version}")
    benchmarks = args.benchmarks
    if not benchmarks:
        benchmarks = [
            name
            for name, value in ALL_BENCHMARKS.items()
            if len(value) > 0 and name not in EXCLUDED
        ]

    for benchmark in benchmarks:
        if benchmark not in ALL_BENCHMARKS:
            print(f"Unknown benchmark: {benchmark}", file=sys.stderr)
            sys.exit(1)

    decorator = identity
    if args.record_py_stats:
        try:
            sys._stats_clear()
        except AttributeError as exc:
            print(
                f"ERROR: {exc} -- did you forget to configure Python with --enable-pystats?",
                file=sys.stderr,
            )
            sys.exit(1)
        decorator = record_stats

    print("Benchmark                     Time      Useful Work")
    results = {}
    for benchmark in benchmarks:
        module_name, func_name, kind, loops, *extra = ALL_BENCHMARKS[benchmark]
        if kind == "pyston":
            continue

        start = time.perf_counter()
        time_sec = run_one_benchmark(benchmark, args.scale, decorator)
        true_time = time.perf_counter() - start
        pct = (time_sec / true_time) * 100

        results[benchmark] = time_sec * 1000

        print(f"{benchmark:<28} {time_sec * 1000:6.1f} ms      ({pct:3.0f}%)")

    if args.record_py_stats:
        sys._stats_dump()
        sys._stats_clear()

    if not args.benchmarks:
        # Compute score
        try:
            import fastmark.baselines as baselines
        except ImportError:
            try:
                import baselines
            except ImportError:
                baselines = None
        if baselines is None:
            print("No baselines found, skipping score computation", file=sys.stderr)
        else:
            import math

            log_ratios = []
            for benchmark, time_ms in results.items():
                if benchmark not in baselines.BASELINES:
                    print(f"Missing baseline for {benchmark}", file=sys.stderr)
                    continue

                baseline = baselines.BASELINES[benchmark]
                log_ratios.append(math.log(baseline / time_ms))

            score = 10000 * math.exp(sum(log_ratios) / len(log_ratios))
            results["score"] = score
            print(f"Score: {score:7.1f}")  # higher is better

    if args.save_baselines:
        import pprint

        with open(args.save_baselines, "w") as f:
            f.write("BASELINES = ")
            pprint.pprint(results, stream=f)

    if args.json:
        import json

        with open(args.json, "w") as f:
            json.dump(results, f, indent=2)


def cli(argv=None):
    parser = argparse.ArgumentParser()
    parser.add_argument(
        "--scale",
        type=int,
        default=100,
        help="work scale factor for the benchmark (default=100)",
    )
    parser.add_argument(
        "--json",
        type=str,
        default=None,
        help="save results as JSON to the specified path",
    )
    parser.add_argument(
        "--record-py-stats",
        default=False,
        action="store_true",
        help="record py stats while benchmarks are running",
    )
    parser.add_argument(
        "--save-baselines", type=str, default=None, help="save results as the baselines"
    )
    parser.add_argument("benchmarks", nargs="*", help="benchmarks to run")
    options = parser.parse_args(argv)
    main(options)


if __name__ == "__main__":
    cli()
