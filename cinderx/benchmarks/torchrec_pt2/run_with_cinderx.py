#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.

"""Run the TorchRec DMP eager forward+backward with CinderX JIT enabled.

Measures CinderX JIT auto-compilation impact on the Python-level orchestration
code in torchrec's distributed embedding sharding (DistributedModelParallel).

Usage (via buck):
    buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx

    # With options:
    buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx -- \\
        --num-features 200 --repeat 3 --iters 50

    # Without JIT (for comparison):
    PYTHONJITDISABLE=1 buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx -- \\
        --num-features 200 --repeat 3 --iters 50

    # Specific Python version:
    buck run //cinderx/benchmarks/torchrec_pt2:run_with_cinderx-314 -- --num-features 200
"""

import sys
import time

import cinderx.jit
import click
import torch
import torch._dynamo
import torchrec
import torchrec.pt2.checks
from torch import distributed as dist
from torch._dynamo.testing import reduce_to_scalar_loss
from torch.distributed import ProcessGroup
from torch.testing._internal.distributed.fake_pg import FakeStore
from torchrec.distributed.embedding import EmbeddingCollectionSharder
from torchrec.distributed.model_parallel import DistributedModelParallel
from torchrec.distributed.planner import EmbeddingShardingPlanner, Topology
from torchrec.distributed.planner.enumerators import EmbeddingEnumerator
from torchrec.distributed.planner.shard_estimators import (
    EmbeddingPerfEstimator,
    EmbeddingStorageEstimator,
)
from torchrec.distributed.planner.types import ShardingPlan
from torchrec.distributed.sharding_plan import EmbeddingBagCollectionSharder
from torchrec.distributed.test_utils.test_model import ModelInput
from torchrec.distributed.types import ShardingEnv, ShardingType
from torchrec.modules.embedding_modules import (
    EmbeddingBagCollection,
    EmbeddingBagConfig,
)
from torchrec.pt2.utils import kjt_for_pt2_tracing
from torchrec.sparse.jagged_tensor import KeyedJaggedTensor, KeyedTensor

from .test_pt2_multiprocess import (
    _gen_model,
    _ModelType,
    _TestConfig,
    EBCSharderFixedShardingType,
    ECSharderFixedShardingType,
    TestModelInfo,
)


def setup_benchmark(
    rank: int = 0,
    world_size: int = 2,
    num_features: int = 5,
    batch_size: int = 10,
    num_embeddings: int = 256,
):
    """Set up the benchmark (model, DMP, inputs). Returns everything needed."""
    sharding_type = ShardingType.TABLE_WISE.value
    emb_dim = 12
    num_float_features: int = 8
    num_weighted_features: int = 1

    device: torch.Device = torch.device("cpu")
    store = FakeStore()
    dist.init_process_group(
        backend="fake", rank=rank, world_size=world_size, store=store
    )
    pg: ProcessGroup = dist.distributed_c10d._get_default_group()

    topology: Topology = Topology(world_size=world_size, compute_device="cpu")
    mi = TestModelInfo(
        # pyrefly: ignore [bad-argument-type]
        dense_device=device,
        # pyrefly: ignore [bad-argument-type]
        sparse_device=device,
        num_features=num_features,
        num_float_features=num_float_features,
        num_weighted_features=num_weighted_features,
        topology=topology,
    )

    mi.planner = EmbeddingShardingPlanner(
        topology=topology,
        batch_size=batch_size,
        enumerator=EmbeddingEnumerator(
            topology=topology,
            batch_size=batch_size,
            estimator=[
                EmbeddingPerfEstimator(topology=topology),
                EmbeddingStorageEstimator(topology=topology),
            ],
        ),
    )

    mi.tables = [
        EmbeddingBagConfig(
            num_embeddings=num_embeddings,
            embedding_dim=emb_dim,
            name="table_" + str(i),
            feature_names=["feature_" + str(i)],
        )
        for i in range(mi.num_features)
    ]

    mi.weighted_tables = [
        EmbeddingBagConfig(
            num_embeddings=num_embeddings,
            embedding_dim=emb_dim,
            name="weighted_table_" + str(i),
            feature_names=["weighted_feature_" + str(i)],
        )
        for i in range(mi.num_weighted_features)
    ]

    mi.model = _gen_model(_ModelType.EBC, mi)
    mi.model.training = True

    planner = EmbeddingShardingPlanner(
        topology=Topology(world_size, device.type),
        constraints=None,
    )

    sharders = [
        EBCSharderFixedShardingType(sharding_type),
        ECSharderFixedShardingType(sharding_type),
    ]

    plan: ShardingPlan = planner.plan(mi.model, sharders)

    dmp = DistributedModelParallel(
        mi.model,
        env=ShardingEnv(world_size, rank, pg),
        plan=plan,
        # pyrefly: ignore [bad-argument-type]
        sharders=sharders,
        # pyrefly: ignore [bad-argument-type]
        device=device,
        init_data_parallel=False,
    )

    _, local_model_inputs = ModelInput.generate(
        batch_size=batch_size,
        world_size=world_size,
        num_float_features=num_float_features,
        tables=mi.tables,
        weighted_tables=mi.weighted_tables,
        variable_batch_size=False,
    )

    # pyrefly: ignore [bad-argument-type]
    local_model_input = local_model_inputs[rank].to(device)
    kjt = local_model_input.idlist_features
    ff = local_model_input.float_features
    ff.requires_grad = True
    # pyrefly: ignore [bad-argument-type]
    kjt_ft = kjt_for_pt2_tracing(kjt, convert_to_vb=False)

    if hasattr(torchrec.distributed, "comm_ops") and hasattr(
        # pyrefly: ignore [implicit-import]
        torchrec.distributed.comm_ops,
        "set_use_sync_collectives",
    ):
        # pyrefly: ignore [implicit-import]
        torchrec.distributed.comm_ops.set_use_sync_collectives(True)
    torchrec.pt2.checks.set_use_torchdynamo_compiling_path(True)

    dmp.train(True)

    return {
        "dmp": dmp,
        "kjt_ft": kjt_ft,
        "ff": ff,
    }


def run_eager_iters(ctx: dict, num_iters: int) -> float:
    """Run timed eager forward+backward iterations. Returns elapsed seconds."""
    dmp = ctx["dmp"]
    kjt_ft = ctx["kjt_ft"]
    ff = ctx["ff"]

    t_start = time.perf_counter()
    for _ in range(num_iters):
        out = dmp(kjt_ft, ff)
        reduce_to_scalar_loss(out).backward()
    t_end = time.perf_counter()
    return t_end - t_start


@click.command()
@click.option("--repeat", type=int, default=3, help="Number of timed runs")
@click.option(
    "--iters", type=int, default=50, help="Forward+backward iterations per run"
)
@click.option("--warmup", type=int, default=5, help="Warmup iterations (not timed)")
@click.option("--rank", type=int, default=0)
@click.option("--world-size", type=int, default=2)
@click.option("--num-features", type=int, default=200)
@click.option("--batch-size", type=int, default=10)
def main(
    rank: int,
    world_size: int,
    num_features: int,
    batch_size: int,
    repeat: int,
    iters: int,
    warmup: int,
):
    print(f"Python {sys.version}")
    print("CinderX JIT eager forward+backward benchmark")
    print(
        f"num_features={num_features}, batch_size={batch_size}, "
        f"warmup={warmup}, iters={iters}, repeat={repeat}"
    )
    print()

    # Setup model and DMP
    print("Setting up model and DMP...")
    ctx = setup_benchmark(
        rank=rank,
        world_size=world_size,
        num_features=num_features,
        batch_size=batch_size,
    )

    # Enable JIT auto-compilation
    print("Enabling CinderX JIT...")
    cinderx.jit.auto()

    # Warmup (lets JIT compile hot functions)
    print(f"Warmup ({warmup} iters)...")
    run_eager_iters(ctx, warmup)

    # Timed runs
    times = []
    for i in range(repeat):
        t = run_eager_iters(ctx, iters)
        times.append(t)
        print(
            f"  Run {i + 1}/{repeat}: {iters} iters in {t:.3f}s "
            f"({t / iters * 1000:.1f}ms/iter)"
        )

    num_compiled_functions = len(cinderx.jit.get_compiled_functions())
    total_compile_time = cinderx.jit.get_compilation_time()

    dist.destroy_process_group()

    avg = sum(times) / len(times)
    avg_per_iter = avg / iters
    print()
    print("=" * 60)
    print("CinderX JIT results (eager forward+backward):")
    print(f"  num_features={num_features}, batch_size={batch_size}")
    print(f"  warmup={warmup}, iters_per_run={iters}, runs={repeat}")
    print(f"  Run times: {[f'{t:.3f}s' for t in times]}")
    print(f"  Avg per run: {avg:.3f}s")
    print(f"  Avg per iter: {avg_per_iter * 1000:.1f}ms")
    print(f"  JIT Compiled Functions: {num_compiled_functions}")
    print(f"  JIT Total Compile Time: {total_compile_time}ms")
    print("=" * 60)


if __name__ == "__main__":
    main()
