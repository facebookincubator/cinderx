# Copyright (c) Meta Platforms, Inc. and affiliates.

# pyre-strict

"""Benchmarks of CPU-bound stages from real PyTorch inference pipelines.

Production PyTorch serving does substantial work on the host CPU, around the
model, transforming a model's inputs and outputs.  This benchmark exercises
several such stages as standalone workloads:

* ``nms``: object-detection post-processing -- anchor box decode + greedy
  non-max suppression over a detector head's outputs.
* ``bpe``: byte-level byte-pair-encoding tokenization -- subword preprocessing
  that turns input text into token ids for NLP/LLM models.
* ``beam``: beam-search decode bookkeeping -- expand, score, and prune
  hypotheses and finalize an n-best list from per-step model logits.
* ``ngram``: generation logits post-processing -- no-repeat-ngram blocking and
  repetition penalty applied to per-step logits.
* ``ctc``: CTC greedy decoding -- collapse repeated frames and drop blanks to
  turn acoustic-model emissions into a token string.
* ``rec``: recommendation feature preprocessing -- hash and bucketize raw
  request features and assemble a KeyedJaggedTensor-style batch.

These are deliberately *pure-Python* implementations.  Most latency-sensitive production
pipelines run these stages in native code -- e.g. NMS via the C++/CUDA
``torchvision.ops.nms`` kernel, tokenization via Rust/C++ libraries (HuggingFace
``tokenizers``, ``tiktoken``, SentencePiece), beam search / sampling fused inside
serving engines (vLLM, TensorRT-LLM), ngram blocking via a fused CUDA kernel (fairseq
``NGramRepeatBlock``), and CTC beam search via the C++ flashlight decoder.  The
pure-Python form still occurs in real systems -- CPU-only or dependency-constrained
serving, on-device, research/prototype code, and the reference implementations those
native libraries were derived from (HuggingFace's beam scorer and ``no_repeat_ngram``
processor, torchaudio's greedy CTC decoder, the TorchRec feature-preprocessing
transforms) -- and isolating it is what lets this benchmark measure interpreter/JIT
performance.

Run a workload, or compare the interpreter against the JIT, with::

    inference_pipeline --workload nms
    inference_pipeline --compare --workload bpe

"""

from __future__ import annotations

import bisect
import heapq
import math
import os
import random
import re
import statistics
import subprocess
import sys
import time
from collections import Counter
from dataclasses import dataclass
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


# ---------------------------------------------------------------------------
# Workload: detection post-processing (anchor decode + greedy NMS)
# ---------------------------------------------------------------------------


class Box:
    """A decoded detection.  ``__slots__`` avoids a per-instance ``__dict__`` and
    speeds attribute access; production keeps boxes as rows in an ``(N, 4)`` tensor
    rather than as Python objects."""

    __slots__ = ("x1", "y1", "x2", "y2", "score")

    def __init__(
        self, x1: float, y1: float, x2: float, y2: float, score: float
    ) -> None:
        self.x1 = x1
        self.y1 = y1
        self.x2 = x2
        self.y2 = y2
        self.score = score


def iou(a: Box, b: Box) -> float:
    ix1 = a.x1 if a.x1 > b.x1 else b.x1
    iy1 = a.y1 if a.y1 > b.y1 else b.y1
    ix2 = a.x2 if a.x2 < b.x2 else b.x2
    iy2 = a.y2 if a.y2 < b.y2 else b.y2
    iw = ix2 - ix1
    ih = iy2 - iy1
    if iw <= 0.0 or ih <= 0.0:
        return 0.0
    inter = iw * ih
    area_a = (a.x2 - a.x1) * (a.y2 - a.y1)
    area_b = (b.x2 - b.x1) * (b.y2 - b.y1)
    return inter / (area_a + area_b - inter)


class NmsWorkload:
    """Decode anchor-relative box regressions, threshold by score, then run
    greedy non-max suppression -- all in pure Python over ``Box`` objects.

    Production detectors do this with vectorized tensor ops and a C++/CUDA NMS
    kernel (``torchvision.ops.batched_nms``); the pure-Python form here mirrors
    CPU-only / no-torchvision / prototype implementations.  The detector-head
    outputs are generated once with a fixed seed in setup, so the timed
    per-request cost is the Python decode + NMS.
    """

    def __init__(
        self, num_anchors: int, score_thresh: float, iou_thresh: float, topk: int
    ) -> None:
        self.score_thresh = score_thresh
        self.iou_thresh = iou_thresh
        self.topk = topk

        rng = random.Random(1234)
        image = 512.0
        side = int(math.isqrt(num_anchors))
        step = image / side
        size = step * 1.5  # base anchor size; >step so neighbours overlap

        # Detector head outputs (the "model" part): per-anchor objectness score
        # and a 4-vector of box regressions.
        count = side * side
        scores = [rng.random() for _ in range(count)]
        deltas = [[rng.gauss(0.0, 1.0) * 0.1 for _ in range(4)] for _ in range(count)]
        anchors: list[tuple[float, float, float, float]] = []
        for row in range(side):
            for col in range(side):
                anchors.append(
                    (col * step + step / 2, row * step + step / 2, size, size)
                )

        self.scores: list[float] = scores
        self.deltas: list[list[float]] = deltas
        self.anchors: list[tuple[float, float, float, float]] = anchors

    def step(self) -> list[Box]:
        score_thresh = self.score_thresh
        boxes: list[Box] = []
        for score, delta, anchor in zip(self.scores, self.deltas, self.anchors):
            if score < score_thresh:
                continue
            ax, ay, aw, ah = anchor
            cx = ax + delta[0] * aw
            cy = ay + delta[1] * ah
            w = aw * math.exp(delta[2])
            h = ah * math.exp(delta[3])
            half_w = w / 2.0
            half_h = h / 2.0
            boxes.append(Box(cx - half_w, cy - half_h, cx + half_w, cy + half_h, score))

        boxes.sort(key=lambda b: b.score, reverse=True)
        del boxes[self.topk :]

        # Greedy NMS with a suppression mask (the idiomatic pure-Python form):
        # walk boxes high-score first, keep each un-suppressed box, and suppress
        # the lower-scoring boxes that overlap it.
        iou_thresh = self.iou_thresh
        suppressed = [False] * len(boxes)
        kept: list[Box] = []
        for i, box in enumerate(boxes):
            if suppressed[i]:
                continue
            kept.append(box)
            for j in range(i + 1, len(boxes)):
                if not suppressed[j] and iou(box, boxes[j]) > iou_thresh:
                    suppressed[j] = True
        return kept


# ---------------------------------------------------------------------------
# Workload: byte-level byte-pair-encoding tokenizer
# ---------------------------------------------------------------------------
#
# A faithful pure-Python byte-level BPE tokenizer in the style of the GPT-2
# reference encoder (and metaseq's gpt2_bpe_utils.py): pre-tokenize the text,
# byte-encode each piece, apply ranked merges with the canonical pair-merge
# loop, map the resulting symbols to integer ids, and cache per piece so a
# repeated piece is tokenized only once.  Production serving uses a Rust/C++
# library (HuggingFace tokenizers, tiktoken, SentencePiece); this mirrors the
# pure-Python reference those were derived from.

_CORPUS: str = (
    "In 2019 the research team released version 3 of their model, trained on "
    "roughly 45 billion tokens scraped from books, articles, and forum posts. "
    "Engineers noted that tokenization, not matrix multiplication, dominated "
    "latency for short requests: splitting text, normalizing punctuation, and "
    "merging byte pairs all happened on the CPU before a single tensor was "
    'allocated. "Measure first," the tech lead wrote, "then optimize the part '
    'that actually costs you something." By April they had rewritten the hot '
    "path twice and cut p99 latency from 18ms to under 4ms. "
)

# Pre-tokenization splitter.  Real GPT-2 uses a Unicode-property regex (the
# ``regex`` module); we approximate it with stdlib ``re``: runs of letters, runs
# of digits, and runs of other non-space characters, each optionally led by a
# single space (GPT-2 keeps the leading space as part of the piece).
_PRETOKEN_RE: re.Pattern[str] = re.compile(r" ?[^\W\d_]+| ?\d+| ?[^\s\w]+|\s+")

_INF: float = float("inf")


def _bytes_to_unicode() -> dict[int, str]:
    """GPT-2's reversible byte<->unicode map, so every byte becomes a printable
    character that BPE can merge."""
    printable = (
        list(range(ord("!"), ord("~") + 1))
        + list(range(ord("¡"), ord("¬") + 1))
        + list(range(ord("®"), ord("ÿ") + 1))
    )
    mapping = {b: chr(b) for b in printable}
    extra = 0
    for b in range(256):
        if b not in mapping:
            mapping[b] = chr(256 + extra)
            extra += 1
    return mapping


def _get_pairs(symbols: list[str]) -> set[tuple[str, str]]:
    return {(symbols[i], symbols[i + 1]) for i in range(len(symbols) - 1)}


def _train_bpe(words: Counter[str], num_merges: int) -> dict[tuple[str, str], int]:
    """A standard greedy BPE training pass: repeatedly merge the most frequent
    adjacent symbol pair, over byte-encoded pieces.  Run once at setup to produce
    the merge ranking; production loads a pretrained merges table instead, so this
    is excluded from the timed loop."""
    splits: dict[str, list[str]] = {word: list(word) for word in words}
    merges: dict[tuple[str, str], int] = {}
    for rank in range(num_merges):
        pair_freq: Counter[tuple[str, str]] = Counter()
        for word, freq in words.items():
            symbols = splits[word]
            for i in range(len(symbols) - 1):
                pair_freq[(symbols[i], symbols[i + 1])] += freq
        if not pair_freq:
            break
        best = max(pair_freq, key=lambda pair: pair_freq[pair])
        merges[best] = rank
        merged = best[0] + best[1]
        for word, symbols in splits.items():
            i = 0
            out: list[str] = []
            while i < len(symbols):
                if i < len(symbols) - 1 and (symbols[i], symbols[i + 1]) == best:
                    out.append(merged)
                    i += 2
                else:
                    out.append(symbols[i])
                    i += 1
            splits[word] = out
    return merges


def _bpe(
    token: str,
    ranks: dict[tuple[str, str], int],
    cache: dict[str, list[str]],
) -> list[str]:
    """Apply ranked merges to one byte-encoded piece, lowest rank first.

    The canonical GPT-2 pair-merge loop, with a per-piece cache so a repeated
    piece is tokenized only once -- the optimization that makes real pure-Python
    tokenizers tractable on natural text, where a handful of words dominate."""
    cached = cache.get(token)
    if cached is not None:
        return cached
    symbols = list(token)
    pairs = _get_pairs(symbols)
    while pairs:
        best = min(pairs, key=lambda pair: ranks.get(pair, _INF))
        if best not in ranks:
            break
        first, second = best
        out: list[str] = []
        i = 0
        while i < len(symbols):
            try:
                j = symbols.index(first, i)
            except ValueError:
                out.extend(symbols[i:])
                break
            out.extend(symbols[i:j])
            if j < len(symbols) - 1 and symbols[j + 1] == second:
                out.append(first + second)
                i = j + 2
            else:
                out.append(symbols[j])
                i = j + 1
        symbols = out
        if len(symbols) == 1:
            break
        pairs = _get_pairs(symbols)
    cache[token] = symbols
    return symbols


class BpeWorkload:
    """Byte-level BPE tokenization of a fixed input text into integer token ids.

    Per request: pre-tokenize the text, byte-encode each piece, apply ranked
    merges, and map symbols to ids.  Production serving uses a Rust/C++ tokenizer;
    this is the pure-Python reference form.  A per-request cache dedupes repeated
    pieces within the text -- so the merge loop runs once per distinct piece and
    repeats are cache hits, the intra-request behavior of a real tokenizer (a
    long-running server would additionally cache across requests).
    """

    def __init__(self, num_merges: int, repeat_text: int) -> None:
        self.byte_encoder: dict[int, str] = _bytes_to_unicode()
        self.text: str = _CORPUS * repeat_text

        pieces: Counter[str] = Counter()
        for piece in _PRETOKEN_RE.findall(_CORPUS):
            pieces[self._byte_encode(piece)] += 1
        self.ranks: dict[tuple[str, str], int] = _train_bpe(pieces, num_merges)

        # Vocabulary: the 256 byte symbols, then one id per learned merge.
        self.token_to_id: dict[str, int] = {
            ch: i for i, ch in enumerate(sorted(set(self.byte_encoder.values())))
        }
        for (first, second), _rank in sorted(self.ranks.items(), key=lambda kv: kv[1]):
            self.token_to_id.setdefault(first + second, len(self.token_to_id))

    def _byte_encode(self, piece: str) -> str:
        encoder = self.byte_encoder
        return "".join(encoder[b] for b in piece.encode("utf-8"))

    def step(self) -> list[int]:
        token_to_id = self.token_to_id
        ranks = self.ranks
        cache: dict[str, list[str]] = {}
        ids: list[int] = []
        for piece in _PRETOKEN_RE.findall(self.text):
            token = self._byte_encode(piece)
            for symbol in _bpe(token, ranks, cache):
                ids.append(token_to_id[symbol])
        return ids


# ---------------------------------------------------------------------------
# Workload: beam-search decode bookkeeping
# ---------------------------------------------------------------------------
#
# Mirrors HuggingFace `BeamSearchScorer` / `BeamHypotheses`: the per-step search
# bookkeeping (expand candidates, route EOS into a bounded n-best list with a
# length penalty, keep the top live beams, finalize) that drives a seq2seq / LLM
# decoder.  The model logits are mocked as precomputed per-step vectors so the
# timed work is the Python search, not the model.  Production engines (vLLM,
# TensorRT-LLM) fuse this with GPU sampling; the per-step top-k over the vocab is
# a tensor op there and is kept in Python here to stay self-contained.


class _BeamHypotheses:
    """Bounded n-best list of finished hypotheses, scored with a length penalty
    (the HuggingFace `BeamHypotheses` algorithm)."""

    def __init__(self, num_beams: int, length_penalty: float) -> None:
        self.num_beams = num_beams
        self.length_penalty = length_penalty
        self.beams: list[tuple[float, list[int]]] = []
        self.worst_score: float = 1e9

    def add(self, tokens: list[int], sum_logprobs: float) -> None:
        score = sum_logprobs / (len(tokens) ** self.length_penalty)
        if len(self.beams) < self.num_beams or score > self.worst_score:
            self.beams.append((score, tokens))
            if len(self.beams) > self.num_beams:
                ordered = sorted((s, i) for i, (s, _) in enumerate(self.beams))
                del self.beams[ordered[0][1]]
                self.worst_score = ordered[1][0]
            else:
                self.worst_score = min(score, self.worst_score)

    def is_done(self, best_sum_logprobs: float, cur_len: int) -> bool:
        if len(self.beams) < self.num_beams:
            return False
        best_possible = best_sum_logprobs / (cur_len**self.length_penalty)
        return self.worst_score >= best_possible


class BeamSearchWorkload:
    """Beam-search decode over mocked per-step logits, producing an n-best list."""

    def __init__(
        self, vocab_size: int, num_beams: int, max_steps: int, length_penalty: float
    ) -> None:
        self.vocab_size = vocab_size
        self.num_beams = num_beams
        self.max_steps = max_steps
        self.length_penalty = length_penalty
        self.eos_id: int = vocab_size - 1

        rng = random.Random(7)
        # Per-step, per-beam-slot log-prob vectors.
        self.step_logits: list[list[list[float]]] = [
            [[rng.gauss(0.0, 1.0) for _ in range(vocab_size)] for _ in range(num_beams)]
            for _ in range(max_steps)
        ]

    def step(self) -> list[tuple[float, list[int]]]:
        num_beams = self.num_beams
        eos = self.eos_id
        # Standard init: only the first beam is active, so step 1 does not pick
        # the same token num_beams times.
        beam_scores: list[float] = [0.0] + [-1e9] * (num_beams - 1)
        beam_tokens: list[list[int]] = [[] for _ in range(num_beams)]
        hyps = _BeamHypotheses(num_beams, self.length_penalty)

        for t in range(self.max_steps):
            logits_t = self.step_logits[t]
            candidates: list[tuple[float, int, int]] = []
            for b in range(len(beam_scores)):
                base = beam_scores[b]
                for token, logprob in enumerate(logits_t[b]):
                    candidates.append((base + logprob, token, b))
            top = heapq.nlargest(2 * num_beams, candidates)

            next_scores: list[float] = []
            next_tokens: list[list[int]] = []
            for score, token, b in top:
                if token == eos:
                    hyps.add(beam_tokens[b] + [token], score)
                else:
                    next_scores.append(score)
                    next_tokens.append(beam_tokens[b] + [token])
                if len(next_scores) == num_beams:
                    break

            beam_scores = next_scores
            beam_tokens = next_tokens
            if len(beam_scores) < num_beams or hyps.is_done(top[0][0], t + 1):
                break

        for tokens, score in zip(beam_tokens, beam_scores):
            hyps.add(tokens, score)
        return sorted(hyps.beams, key=lambda b: b[0], reverse=True)[:num_beams]


# ---------------------------------------------------------------------------
# Workload: generation logits post-processing (no-repeat-ngram + rep. penalty)
# ---------------------------------------------------------------------------
#
# Mirrors HuggingFace `NoRepeatNGramLogitsProcessor` + `RepetitionPenaltyLogits
# Processor`, applied to each hypothesis's logits every decode step: rebuild the
# n-gram dict from the generated sequence, ban tokens that would repeat an
# n-gram, and penalize already-seen tokens.  fairseq additionally ships a fused
# C++/CUDA `NGramRepeatBlock`; the HuggingFace path benchmarked here is pure
# Python.


class NgramLogitsWorkload:
    def __init__(
        self,
        num_seqs: int,
        seq_len: int,
        vocab_size: int,
        ngram_size: int,
        penalty: float,
    ) -> None:
        self.ngram_size = ngram_size
        self.penalty = penalty

        rng = random.Random(13)
        self.sequences: list[list[int]] = [
            [rng.randrange(vocab_size) for _ in range(seq_len)] for _ in range(num_seqs)
        ]
        self.logits: list[list[float]] = [
            [rng.gauss(0.0, 1.0) for _ in range(vocab_size)] for _ in range(num_seqs)
        ]

    def step(self) -> list[list[float]]:
        n = self.ngram_size
        penalty = self.penalty
        results: list[list[float]] = []
        for seq, base_logits in zip(self.sequences, self.logits):
            scores = base_logits[:]
            # no-repeat-ngram: ban tokens that would complete a seen n-gram.
            if len(seq) + 1 >= n:
                ngrams: dict[tuple[int, ...], list[int]] = {}
                for ngram in zip(*[seq[i:] for i in range(n)]):
                    prefix = ngram[:-1]
                    ngrams.setdefault(prefix, []).append(ngram[-1])
                lookup = tuple(seq[len(seq) + 1 - n :])
                for token in ngrams.get(lookup, ()):
                    scores[token] = -math.inf
            # repetition penalty over already-generated tokens.
            for token in set(seq):
                value = scores[token]
                scores[token] = value / penalty if value > 0 else value * penalty
            results.append(scores)
        return results


# ---------------------------------------------------------------------------
# Workload: CTC greedy decoding
# ---------------------------------------------------------------------------
#
# Mirrors the pure-Python greedy CTC decoder used in torchaudio's wav2letter pipeline
# `greedy_search`: collapse runs of equal argmax ids, drop the blank id, and map the
# rest to tokens.  The accurate beam-search CTC decoder (with an n-gram LM) is a native
# C++ flashlight extension; greedy decoding is the pure-Python path.


class CtcGreedyWorkload:
    def __init__(self, num_utterances: int, num_frames: int, vocab_size: int) -> None:
        self.blank_id = 0
        self.id_to_token: dict[int, str] = {
            i: chr(ord("a") + i - 1) for i in range(1, vocab_size)
        }

        rng = random.Random(17)
        self.frames: list[list[int]] = [
            [rng.randrange(vocab_size) for _ in range(num_frames)]
            for _ in range(num_utterances)
        ]

    def step(self) -> list[str]:
        blank = self.blank_id
        id_to_token = self.id_to_token
        out: list[str] = []
        for ids in self.frames:
            tokens: list[str] = []
            prev = -1
            for token_id in ids:
                if token_id != blank and token_id != prev:
                    tokens.append(id_to_token[token_id])
                prev = token_id
            out.append("".join(tokens))
        return out


# ---------------------------------------------------------------------------
# Workload: recommendation feature preprocessing
# ---------------------------------------------------------------------------
#
# Mirrors feature-preprocessing transforms and TorchRec KeyedJaggedTensor assembly: hash
# sparse categorical ids into buckets, clip each example's id list and append a bias id,
# bucketize dense features, and build the flat values/lengths/offsets of a jagged batch.
# This host-side glue is genuinely Python in real serving (there is no single fused
# native op spanning the per-feature chain + request->KJT collation).


def _sigrid_hash(value: int, modulo: int) -> int:
    """Sigrid's multiplicative bit-mix hash, reduced into ``[0, modulo)``."""
    mixed = (value * 0x52DCE729) & 0xFFFFFFFFFFFFFFFF
    mixed ^= mixed >> 47
    combined = mixed ^ 0xF3
    return (combined & 0x7FFFFFFF) % modulo


class RecPreprocWorkload:
    def __init__(
        self,
        batch_size: int,
        num_sparse: int,
        num_dense: int,
        num_buckets: int,
        max_len: int,
    ) -> None:
        self.max_len = max_len
        rng = random.Random(19)

        self.modulos: list[int] = [num_buckets] * num_sparse
        self.bias_ids: list[int] = [num_buckets + f for f in range(num_sparse)]

        # Per sparse feature: a ragged batch of raw categorical ids.
        self.sparse_raw: list[list[list[int]]] = []
        for _f in range(num_sparse):
            feature: list[list[int]] = []
            for _ in range(batch_size):
                length = rng.randrange(1, max_len + 4)
                feature.append([rng.randrange(10_000_000) for _ in range(length)])
            self.sparse_raw.append(feature)

        # Per dense feature: a batch of values and sorted bucket borders.
        self.dense_raw: list[list[float]] = [
            [rng.gauss(0.0, 1.0) for _ in range(batch_size)] for _ in range(num_dense)
        ]
        self.borders: list[list[float]] = [
            sorted(rng.gauss(0.0, 1.0) for _ in range(31)) for _ in range(num_dense)
        ]

    def step(self) -> tuple[dict[int, tuple[list[int], list[int]]], list[list[int]]]:
        max_len = self.max_len
        sparse: dict[int, tuple[list[int], list[int]]] = {}
        for feature, (raw_lists, modulo, bias) in enumerate(
            zip(self.sparse_raw, self.modulos, self.bias_ids)
        ):
            values: list[int] = []
            offsets: list[int] = [0]
            for ids in raw_lists:
                clipped = ids if len(ids) <= max_len else ids[:max_len]
                for raw in clipped:
                    values.append(_sigrid_hash(raw, modulo))
                values.append(bias)  # per-example bias term
                offsets.append(len(values))
            sparse[feature] = (values, offsets)

        dense_buckets: list[list[int]] = []
        for column, borders in zip(self.dense_raw, self.borders):
            dense_buckets.append([bisect.bisect_left(borders, x) for x in column])
        return sparse, dense_buckets


# ---------------------------------------------------------------------------
# Workload registry
# ---------------------------------------------------------------------------


@dataclass(frozen=True)
class Workload:
    description: str
    build: Callable[[], object]


WORKLOADS: dict[str, Workload] = {
    "nms": Workload(
        "anchor box decode + greedy non-max suppression",
        lambda: NmsWorkload(
            num_anchors=2000, score_thresh=0.5, iou_thresh=0.5, topk=200
        ),
    ),
    "bpe": Workload(
        "byte-level byte-pair-encoding tokenization",
        lambda: BpeWorkload(num_merges=200, repeat_text=4),
    ),
    "beam": Workload(
        "beam-search decode bookkeeping",
        lambda: BeamSearchWorkload(
            vocab_size=128, num_beams=4, max_steps=32, length_penalty=1.0
        ),
    ),
    "ngram": Workload(
        "no-repeat-ngram + repetition-penalty logits processing",
        lambda: NgramLogitsWorkload(
            num_seqs=16, seq_len=192, vocab_size=512, ngram_size=3, penalty=1.2
        ),
    ),
    "ctc": Workload(
        "CTC greedy decoding",
        lambda: CtcGreedyWorkload(num_utterances=32, num_frames=384, vocab_size=29),
    ),
    "rec": Workload(
        "recommendation feature preprocessing + jagged-batch assembly",
        lambda: RecPreprocWorkload(
            batch_size=128, num_sparse=16, num_dense=16, num_buckets=100000, max_len=20
        ),
    ),
}


# ---------------------------------------------------------------------------
# Harness
# ---------------------------------------------------------------------------


def run_iterations(step: Callable[[], object], iterations: int) -> float:
    start = time.perf_counter()
    for _ in range(iterations):
        step()
    return time.perf_counter() - start


def run(
    step: Callable[[], object], iterations: int, warmup: int, repeat: int
) -> list[float]:
    print(f"Warmup ({warmup} iterations)...", file=sys.stderr)
    for _ in range(warmup):
        step()

    print(f"Timed runs ({repeat} x {iterations} iterations)...", file=sys.stderr)
    samples_ms: list[float] = []
    for i in range(repeat):
        elapsed = run_iterations(step, iterations)
        mean_ms = elapsed / iterations * 1000
        samples_ms.append(mean_ms)
        print(f"  Run {i + 1}/{repeat}: {mean_ms:.3f} ms/iter", file=sys.stderr)
    return samples_ms


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
    return [workload] if workload else list(WORKLOADS)


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

    names = [n for n in WORKLOADS if n in baseline and n in jit]
    print(f"\n{'=' * 56}")
    print(f"{'workload':<10}{'baseline':>12}{'jit':>12}{'speedup':>12}")
    print("-" * 56)
    ratios: list[float] = []
    for name in names:
        base_ms = baseline[name]
        jit_ms = jit[name]
        speedup = base_ms / jit_ms if jit_ms else float("nan")
        ratios.append(speedup)
        print(f"{name:<10}{base_ms:>11.3f}m{jit_ms:>11.3f}m{speedup:>11.2f}x")
    print("-" * 56)
    if ratios:
        geomean = math.exp(sum(math.log(r) for r in ratios) / len(ratios))
        print(f"{'geomean':<10}{'':>12}{'':>12}{geomean:>11.2f}x")
    print(f"{'=' * 56}  (higher is better)")


def print_results(
    workload: str,
    warmup: int,
    iterations: int,
    repeat: int,
    enable_cinderx: bool,
    samples_ms: list[float],
) -> float:
    mean_ms = sum(samples_ms) / len(samples_ms)
    median_ms = statistics.median(samples_ms)

    print("", file=sys.stderr)
    print("=" * 60, file=sys.stderr)
    print(f"workload={workload}", file=sys.stderr)
    print(
        f"  warmup={warmup} iterations_per_run={iterations} runs={repeat}",
        file=sys.stderr,
    )
    print(
        f"  run times: {[f'{sample:.3f}ms' for sample in samples_ms]}", file=sys.stderr
    )
    print(f"  mean: {mean_ms:.3f} ms/iter", file=sys.stderr)
    print(f"  median: {median_ms:.3f} ms/iter", file=sys.stderr)
    print(f"  JIT requested={'yes' if enable_cinderx else 'no'}", file=sys.stderr)
    print("=" * 60, file=sys.stderr)
    # Machine-parseable line consumed by --compare (one per workload).
    print(f"result: {workload} {mean_ms:.4f}", file=sys.stderr)
    return mean_ms


@click.command(context_settings={"help_option_names": ["-h", "--help"]})
@click.option(
    "--cinderx", "enable_cinderx", is_flag=True, help="Enable the CinderX JIT"
)
@click.option(
    "--workload",
    type=click.Choice(sorted(WORKLOADS)),
    default=None,
    help="Run a single inference-pipeline stage; if omitted, run all of them",
)
@click.option(
    "--iterations",
    type=click.IntRange(min=1),
    default=2000,
    show_default=True,
    help="Number of timed iterations per run",
)
@click.option(
    "--warmup",
    type=click.IntRange(min=0),
    default=1500,
    show_default=True,
    help="Number of warmup iterations before timing",
)
@click.option(
    "--repeat",
    type=click.IntRange(min=1),
    default=5,
    show_default=True,
    help="Number of timed runs",
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
def cli(
    enable_cinderx: bool,
    workload: str | None,
    iterations: int,
    warmup: int,
    repeat: int,
    compile_after_n_calls: int | None,
    compare: bool,
) -> None:
    if compare:
        run_compare(sys.argv[1:])
        return

    names = select_workloads(workload)
    print(f"Python {sys.version.split()[0]}", file=sys.stderr)
    print(
        f"CinderX inference-pipeline benchmark ({len(names)} workload(s)) "
        f"warmup={warmup} iterations={iterations} repeat={repeat}",
        file=sys.stderr,
    )

    if enable_cinderx:
        print("Enabling the CinderX JIT", file=sys.stderr)
        cinderx.jit.auto()
        if compile_after_n_calls is not None:
            cinderx.jit.compile_after_n_calls(compile_after_n_calls)

    means: dict[str, float] = {}
    for name in names:
        print(
            f"\nSetting up workload {name} ({WORKLOADS[name].description})...",
            file=sys.stderr,
        )
        instance = WORKLOADS[name].build()
        step: Callable[[], object] = instance.step  # pyre-ignore[16]
        samples_ms = run(step, iterations, warmup, repeat)
        means[name] = print_results(
            name, warmup, iterations, repeat, enable_cinderx, samples_ms
        )

    if len(means) > 1:
        print(f"\n{'=' * 36}", file=sys.stderr)
        print("Summary (mean ms/iter)", file=sys.stderr)
        for name, mean_ms in means.items():
            print(f"  {name:<10}{mean_ms:>10.3f}", file=sys.stderr)
        print(f"{'=' * 36}", file=sys.stderr)


if __name__ == "__main__":
    cli()
