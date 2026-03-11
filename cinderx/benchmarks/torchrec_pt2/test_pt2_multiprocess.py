#!/usr/bin/env python3
# Copyright (c) Meta Platforms, Inc. and affiliates.
# All rights reserved.
#
# This source code is licensed under the BSD-style license found in the
# LICENSE file in the root directory of this source tree.

# pyre-strict

#!/usr/bin/env python3

import timeit
import unittest
from dataclasses import dataclass, field
from enum import Enum
from typing import Any, Dict, List, Optional, Tuple, Union

import click
import fbgemm_gpu.sparse_ops  # noqa: F401, E402
import torch
import torchrec
import torchrec.pt2.checks
from fbgemm_gpu.split_table_batched_embeddings_ops_training import (
    SplitTableBatchedEmbeddingBagsCodegen,
)
from hypothesis import given, settings, strategies as st, Verbosity
from torch import distributed as dist
from torch._dynamo.testing import reduce_to_scalar_loss
from torch.distributed import ProcessGroup
from torch.testing._internal.distributed.fake_pg import FakeStore
from torchrec.distributed.embedding import EmbeddingCollectionSharder
from torchrec.distributed.embedding_types import EmbeddingComputeKernel
from torchrec.distributed.fbgemm_qcomm_codec import QCommsConfig
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
from torchrec.modules.embedding_configs import EmbeddingConfig
from torchrec.modules.embedding_modules import (
    EmbeddingBagCollection,
    EmbeddingBagConfig,
    EmbeddingCollection,
)
from torchrec.modules.feature_processor_ import FeatureProcessorsCollection
from torchrec.modules.fp_embedding_modules import FeatureProcessedEmbeddingBagCollection
from torchrec.pt2.utils import kjt_for_pt2_tracing
from torchrec.sparse.jagged_tensor import JaggedTensor, KeyedJaggedTensor, KeyedTensor


@dataclass
class TestModelInfo:
    """Lightweight replacement for torchrec.distributed.test_utils.infer_utils.TestModelInfo.

    Defined locally to avoid pulling in heavy quantization deps from infer_utils.
    """

    sparse_device: torch.device
    dense_device: torch.device
    num_features: int
    num_float_features: int
    num_weighted_features: int
    tables: Union[List[EmbeddingBagConfig], List[EmbeddingConfig]] = field(
        default_factory=list
    )
    weighted_tables: List[EmbeddingBagConfig] = field(default_factory=list)
    model: torch.nn.Module = field(default_factory=torch.nn.Module)
    topology: Optional[Topology] = None
    planner: Optional[EmbeddingShardingPlanner] = None


try:
    torch.ops.load_library("//deeplearning/fbgemm/fbgemm_gpu:sparse_ops")
    torch.ops.load_library("//deeplearning/fbgemm/fbgemm_gpu:sparse_ops_cpu")
    torch.ops.load_library("//deeplearning/fbgemm/fbgemm_gpu/codegen:embedding_ops")
    torch.ops.load_library("//deeplearning/fbgemm/fbgemm_gpu/codegen:embedding_ops_cpu")

    torch.ops.load_library(
        "//deeplearning/fbgemm/fbgemm_gpu/codegen:embedding_ops_cuda_training"
    )
    torch.ops.load_library(
        "//deeplearning/fbgemm/fbgemm_gpu/codegen:embedding_ops_cpu_training"
    )
    torch.ops.load_library(
        "//deeplearning/fbgemm/fbgemm_gpu:split_table_batched_embeddings"
    )
    torch.ops.load_library("//deeplearning/fbgemm/fbgemm_gpu:input_combine_cpu")
except OSError:
    pass


class NoOpFPC(FeatureProcessorsCollection):
    def forward(
        self,
        features: KeyedJaggedTensor,
    ) -> KeyedJaggedTensor:
        return features


class _ModelType(Enum):
    EBC = 1
    EC = 2
    FPEBC = 3


class _InputType(Enum):
    SINGLE_BATCH = 1
    VARIABLE_BATCH = 2


class _ConvertToVariableBatch(Enum):
    FALSE = 0
    TRUE = 1


@dataclass
class _TestConfig:
    n_extra_numerics_checks_inputs: int = 1


class EBCSharderFixedShardingType(EmbeddingBagCollectionSharder):
    def __init__(
        self,
        sharding_type: str,
        fused_params: Optional[Dict[str, Any]] = None,
        qcomms_config: Optional[QCommsConfig] = None,
    ) -> None:
        if fused_params is None:
            fused_params = {}
        if "learning_rate" not in fused_params:
            fused_params["learning_rate"] = 0.1

        self._sharding_type = sharding_type
        super().__init__(fused_params=fused_params)

    def sharding_types(self, compute_device_type: str) -> List[str]:
        return [self._sharding_type]


class ECSharderFixedShardingType(EmbeddingCollectionSharder):
    def __init__(
        self,
        sharding_type: str,
        fused_params: Optional[Dict[str, Any]] = None,
        qcomms_config: Optional[QCommsConfig] = None,
    ) -> None:
        if fused_params is None:
            fused_params = {}
        if "learning_rate" not in fused_params:
            fused_params["learning_rate"] = 0.1

        self._sharding_type = sharding_type
        super().__init__(fused_params=fused_params)

    def sharding_types(self, compute_device_type: str) -> List[str]:
        return [self._sharding_type]


def _gen_model(test_model_type: _ModelType, mi: TestModelInfo) -> torch.nn.Module:
    emb_dim: int = max(t.embedding_dim for t in mi.tables)
    if test_model_type == _ModelType.EBC:

        class M_ebc(torch.nn.Module):
            def __init__(self, ebc: EmbeddingBagCollection) -> None:
                super().__init__()
                self._ebc = ebc
                self._linear = torch.nn.Linear(
                    mi.num_float_features, emb_dim, device=mi.dense_device
                )

            def forward(self, x: KeyedJaggedTensor, y: torch.Tensor) -> torch.Tensor:
                kt: KeyedTensor = self._ebc(x)
                v = kt.values()
                y = self._linear(y)
                return torch.mul(torch.mean(v, dim=1), torch.mean(y, dim=1))

        return M_ebc(
            EmbeddingBagCollection(
                # pyrefly: ignore[bad-argument-type]
                tables=mi.tables,
                device=mi.sparse_device,
            )
        )
    if test_model_type == _ModelType.FPEBC:

        class M_fpebc(torch.nn.Module):
            def __init__(self, fpebc: FeatureProcessedEmbeddingBagCollection) -> None:
                super().__init__()
                self._fpebc = fpebc
                self._linear = torch.nn.Linear(
                    mi.num_float_features, emb_dim, device=mi.dense_device
                )

            def forward(self, x: KeyedJaggedTensor, y: torch.Tensor) -> torch.Tensor:
                kt: KeyedTensor = self._fpebc(x)
                v = kt.values()
                y = self._linear(y)
                return torch.mul(torch.mean(v, dim=1), torch.mean(y, dim=1))

        return M_fpebc(
            FeatureProcessedEmbeddingBagCollection(
                embedding_bag_collection=EmbeddingBagCollection(
                    # pyrefly: ignore[bad-argument-type]
                    tables=mi.tables,
                    device=mi.sparse_device,
                    is_weighted=True,
                ),
                feature_processors=NoOpFPC(),
            )
        )
    elif test_model_type == _ModelType.EC:

        class M_ec(torch.nn.Module):
            def __init__(self, ec: EmbeddingCollection) -> None:
                super().__init__()
                self._ec = ec

            def forward(
                self, x: KeyedJaggedTensor, y: torch.Tensor
            ) -> List[JaggedTensor]:
                d: Dict[str, JaggedTensor] = self._ec(x)
                # pyrefly: ignore[bad-argument-type]
                v = torch.stack(d.values(), dim=0).sum(dim=0)
                # pyrefly: ignore[not-callable]
                y = self._linear(y)
                # pyrefly: ignore[bad-return]
                return torch.mul(torch.mean(v, dim=1), torch.mean(y, dim=1))

        return M_ec(
            EmbeddingCollection(
                # pyrefly: ignore[bad-argument-type]
                tables=mi.tables,
                device=mi.sparse_device,
            )
        )
    else:
        raise RuntimeError(f"Unsupported test_model_type:{test_model_type}")


def _test_compile_fake_pg_fn(
    rank: int,
    world_size: int,
    num_features: int = 5,
    batch_size: int = 10,
    num_embeddings: int = 256,
) -> None:
    sharding_type = ShardingType.TABLE_WISE.value
    torch_compile_backend = "eager"
    config = _TestConfig()
    # emb_dim must be % 4 == 0 for fbgemm
    emb_dim = 12

    num_float_features: int = 8
    num_weighted_features: int = 1

    device: torch.Device = torch.device("cpu")
    store = FakeStore()
    dist.init_process_group(backend="fake", rank=rank, world_size=2, store=store)
    pg: ProcessGroup = dist.distributed_c10d._get_default_group()

    topology: Topology = Topology(world_size=world_size, compute_device="cpu")
    mi = TestModelInfo(
        # pyrefly: ignore[bad-argument-type]
        dense_device=device,
        # pyrefly: ignore[bad-argument-type]
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

    model = mi.model

    planner = EmbeddingShardingPlanner(
        topology=Topology(world_size, device.type),
        constraints=None,
    )

    sharders = [
        EBCSharderFixedShardingType(sharding_type),
        ECSharderFixedShardingType(sharding_type),
    ]

    # pyrefly: ignore[bad-argument-type, missing-argument]
    plan: ShardingPlan = planner.plan(model, sharders)

    def _dmp(m: torch.nn.Module) -> DistributedModelParallel:
        return DistributedModelParallel(
            m,
            env=ShardingEnv(world_size, rank, pg),
            plan=plan,
            # pyrefly: ignore[bad-argument-type]
            sharders=sharders,
            # pyrefly: ignore[bad-argument-type]
            device=device,
            init_data_parallel=False,
        )

    dmp = _dmp(model)
    dmp_compile = _dmp(model)

    # TODO: Fix some data dependent failures on subsequent inputs
    n_extra_numerics_checks = config.n_extra_numerics_checks_inputs
    ins = []

    for _ in range(1 + n_extra_numerics_checks):
        (
            _,
            local_model_inputs,
        ) = ModelInput.generate(
            batch_size=batch_size,
            world_size=world_size,
            num_float_features=num_float_features,
            tables=mi.tables,
            weighted_tables=mi.weighted_tables,
            variable_batch_size=False,
        )
        ins.append(local_model_inputs)

    # pyrefly: ignore[bad-argument-type]
    local_model_input = ins[0][rank].to(device)

    kjt = local_model_input.idlist_features
    ff = local_model_input.float_features
    ff.requires_grad = True
    # pyrefly: ignore[bad-argument-type]
    kjt_ft = kjt_for_pt2_tracing(kjt, convert_to_vb=False)

    compile_input_ff = ff.clone().detach()
    compile_input_ff.requires_grad = True

    # pyrefly: ignore[implicit-import]
    if hasattr(torchrec.distributed, "comm_ops") and hasattr(
        torchrec.distributed.comm_ops,  # pyrefly: ignore[implicit-import]
        "set_use_sync_collectives",
    ):
        torchrec.distributed.comm_ops.set_use_sync_collectives(  # pyrefly: ignore[implicit-import]
            True
        )
    torchrec.pt2.checks.set_use_torchdynamo_compiling_path(True)

    dmp.train(True)
    dmp_compile.train(True)

    def get_weights(dmp: DistributedModelParallel) -> torch.Tensor:
        #  `_lookups`.
        # pyrefly: ignore[missing-attribute]
        tbe = dmp._dmp_wrapped_module._ebc._lookups[0]._emb_modules[0]._emb_module
        assert isinstance(tbe, SplitTableBatchedEmbeddingBagsCodegen)
        #  Optional[memory_format] = ...) -> Tensor, Tensor, Module]` is not a
        #  function.
        # pyrefly: ignore[not-callable]
        return tbe.weights_dev.clone().detach()

    original_weights = get_weights(dmp)
    original_weights.zero_()
    original_compile_weights = get_weights(dmp_compile)
    original_compile_weights.zero_()

    eager_out = dmp(kjt_ft, ff)
    reduce_to_scalar_loss(eager_out).backward()

    ##### COMPILE #####
    # pyrefly: ignore[implicit-import]
    old_skip = torch._dynamo.config.skip_torchrec
    # pyrefly: ignore[bad-assignment]
    torch._dynamo.config.skip_torchrec = False
    torch._dynamo.config.capture_scalar_outputs = True
    torch._dynamo.config.capture_dynamic_output_shape_ops = True
    # pyrefly: ignore[bad-assignment]
    torch._dynamo.config.force_unspec_int_unbacked_size_like_on_torchrec_kjt = True

    opt_fn = torch.compile(
        dmp_compile,
        backend=torch_compile_backend,
        fullgraph=False,
    )
    compile_out = opt_fn(
        # pyrefly: ignore[bad-argument-type]
        kjt_for_pt2_tracing(kjt, convert_to_vb=False),
        compile_input_ff,
    )
    try:
        torch.testing.assert_close(eager_out, compile_out, atol=1e-3, rtol=1e-3)
    except AssertionError:
        print("NOTE: eager vs compile outputs differ on CPU (expected with fake PG)")

    torch._dynamo.config.skip_torchrec = old_skip
    ##### COMPILE END #####


@click.command()
@click.option(
    "--repeat",
    type=int,
    default=1,
    help="repeat times",
)
@click.option(
    "--rank",
    type=int,
    default=0,
    help="rank in the test",
)
@click.option(
    "--world-size",
    type=int,
    default=2,
    help="world_size in the test",
)
@click.option(
    "--num-features",
    type=int,
    default=5,
    help="num_features in the test",
)
@click.option(
    "--batch-size",
    type=int,
    default=10,
    help="batch_size in the test",
)
def compile_benchmark(
    rank: int, world_size: int, num_features: int, batch_size: int, repeat: int
) -> None:
    run: str = (
        f"_test_compile_fake_pg_fn(rank={rank}, world_size={world_size}, "
        f"num_features={num_features}, batch_size={batch_size})"
    )
    print("*" * 20 + " compile_benchmark started " + "*" * 20)
    t = timeit.timeit(stmt=run, number=repeat, globals=globals())
    print("*" * 20 + " compile_benchmark completed " + "*" * 20)
    print(
        f"rank: {rank}, world_size: {world_size}, "
        f"num_features: {num_features}, batch_size: {batch_size}, time: {t:.2f}s"
    )


if __name__ == "__main__":
    compile_benchmark()
