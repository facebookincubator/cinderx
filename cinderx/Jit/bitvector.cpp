// Copyright (c) Meta Platforms, Inc. and affiliates.

#include "cinderx/Jit/bitvector.h"

#include "cinderx/Common/log.h"

#include <algorithm>
#include <bit>
#include <cstdint>

namespace cinderx::jit::util {

namespace {

constexpr size_t kShortBitWidth = sizeof(uintptr_t) * CHAR_BIT;
constexpr size_t kChunkBitWidth = sizeof(uint64_t) * CHAR_BIT;

// Get the number of chunks needed to fit a specific bit width.
constexpr size_t chunksForBits(size_t num_bits) {
  return num_bits / kChunkBitWidth + (num_bits % kChunkBitWidth == 0 ? 0 : 1);
}

} // namespace

BitVector::~BitVector() {
  if (!isShortVector()) {
    delete bit_vec;
  }
}

BitVector::BitVector(size_t num_bits) : num_bits_{num_bits} {
  if (isShortVector()) {
    bits = 0;
  } else {
    bit_vec = new std::vector<uint64_t>(chunksForBits(num_bits), 0);
  }
}

BitVector::BitVector(const BitVector& bv) {
  *this = bv;
}

BitVector::BitVector(BitVector&& bv) noexcept {
  *this = std::move(bv);
}

BitVector& BitVector::operator=(const BitVector& rhs) {
  if (this == &rhs) {
    return *this;
  }

  bool lhs_short = isShortVector();
  bool rhs_short = rhs.isShortVector();

  num_bits_ = rhs.num_bits_;

  if (lhs_short && rhs_short) {
    bits = rhs.bits;
  } else if (lhs_short && !rhs_short) {
    bit_vec = new std::vector<uint64_t>(*rhs.bit_vec);
  } else if (!lhs_short && rhs_short) {
    delete bit_vec;
    bits = rhs.bits;
  } else { // if (!lhs_short && !rhs_short)
    *bit_vec = *rhs.bit_vec;
  }

  return *this;
}

BitVector& BitVector::operator=(BitVector&& rhs) noexcept {
  if (this == &rhs) {
    return *this;
  }

  if (!isShortVector()) {
    delete bit_vec;
  }

  // Doesn't matter which union member is being written here.
  num_bits_ = rhs.num_bits_;
  bits = rhs.bits;
  rhs.num_bits_ = 0;
  rhs.bits = 0;

  return *this;
}

bool BitVector::operator==(const BitVector& rhs) const {
  JIT_THROW_IF(
      num_bits_ != rhs.num_bits_,
      "Comparing bitvectors of different widths, {} and {}",
      num_bits_,
      rhs.num_bits_);
  auto cs = chunks();
  return std::equal(cs.begin(), cs.end(), rhs.chunks().begin());
}

bool BitVector::operator!=(const BitVector& rhs) const {
  return !(*this == rhs);
}

template <typename Op>
BitVector BitVector::binaryOp(const BitVector& rhs, const Op& op) const {
  JIT_THROW_IF(
      num_bits_ != rhs.num_bits_,
      "Binary operation on bitvectors of different widths, {} and {}",
      num_bits_,
      rhs.num_bits_);

  if (isShortVector()) {
    return BitVector{num_bits_, op(bits, rhs.bits)};
  }

  BitVector bv;
  bv.num_bits_ = num_bits_;
  bv.bit_vec = new std::vector<uint64_t>{};
  bv.bit_vec->reserve(bit_vec->size());
  std::transform(
      bit_vec->begin(),
      bit_vec->end(),
      rhs.bit_vec->begin(),
      std::back_inserter(*bv.bit_vec),
      op);
  return bv;
}

BitVector BitVector::operator&(const BitVector& rhs) const {
  return binaryOp(
      rhs, [](uint64_t a, uint64_t b) -> uint64_t { return a & b; });
}

BitVector BitVector::operator|(const BitVector& rhs) const {
  return binaryOp(
      rhs, [](uint64_t a, uint64_t b) -> uint64_t { return a | b; });
}

BitVector BitVector::operator-(const BitVector& rhs) const {
  return binaryOp(
      rhs, [](uint64_t a, uint64_t b) -> uint64_t { return a & ~b; });
}

template <typename Op>
BitVector& BitVector::binaryOpAssign(const BitVector& rhs, const Op& op) {
  JIT_THROW_IF(
      num_bits_ != rhs.num_bits_,
      "Binary operation on bitvectors of different widths, {} and {}",
      num_bits_,
      rhs.num_bits_);

  if (isShortVector()) {
    bits = op(bits, rhs.bits);
  } else {
    std::transform(
        bit_vec->begin(),
        bit_vec->end(),
        rhs.bit_vec->begin(),
        bit_vec->begin(),
        op);
  }

  return *this;
}

BitVector& BitVector::operator&=(const BitVector& rhs) {
  return binaryOpAssign(
      rhs, [](uint64_t a, uint64_t b) -> uint64_t { return a & b; });
}

BitVector& BitVector::operator|=(const BitVector& rhs) {
  return binaryOpAssign(
      rhs, [](uint64_t a, uint64_t b) -> uint64_t { return a | b; });
}

BitVector& BitVector::operator-=(const BitVector& rhs) {
  return binaryOpAssign(
      rhs, [](uint64_t a, uint64_t b) -> uint64_t { return a & ~b; });
}

void BitVector::resetAll() {
  for (uint64_t& chunk : chunks()) {
    chunk = 0;
  }
}

void BitVector::setAll() {
  auto cs = chunks();
  for (uint64_t& chunk : cs) {
    chunk = ~uint64_t{0};
  }

  auto remainder = num_bits_ % kChunkBitWidth;
  if (cs.size() > 0 && remainder != 0) {
    cs.back() = (uint64_t{1} << remainder) - 1;
  }
}

void BitVector::fill(bool v) {
  if (v) {
    setAll();
  } else {
    resetAll();
  }
}

bool BitVector::getBit(size_t bit) const {
  JIT_THROW_IF(
      bit >= num_bits_,
      "BitVector::getBit() called on bit {} for vector of size {}",
      bit,
      num_bits_);
  size_t chunk = bit / kChunkBitWidth;
  size_t offset = bit % kChunkBitWidth;
  return chunks()[chunk] & (uint64_t{1} << offset);
}

void BitVector::setBit(size_t bit, bool v) {
  JIT_THROW_IF(
      bit >= num_bits_,
      "BitVector::setBit() called on bit {} for vector of size {}",
      bit,
      num_bits_);

  if (isShortVector()) {
    auto b = uintptr_t{1} << bit;
    bits = v ? (bits | b) : (bits & ~b);
  } else {
    size_t index = bit / kChunkBitWidth;
    size_t offset = bit % kChunkBitWidth;
    auto& val = bit_vec->at(index);
    auto b = uint64_t{1} << offset;
    val = v ? (val | b) : (val & ~b);
  }
}

uintptr_t BitVector::shortBits() const {
  JIT_THROW_IF(
      !isShortVector(), "BitVector::shortBits() called on large vector");
  return bits;
}

void BitVector::setShortBits(uintptr_t new_bits) {
  JIT_THROW_IF(
      !isShortVector(),
      "BitVector::setShortBits() with value {} on large vector of size {}",
      new_bits,
      num_bits_);
  JIT_THROW_IF(
      num_bits_ != kShortBitWidth &&
          (new_bits & ~((uintptr_t{1} << num_bits_) - 1)) != 0,
      "BitVector::setShortBits() with value {} can't fit in vector of size {}",
      new_bits,
      num_bits_);
  bits = new_bits;
}

uint64_t BitVector::getBitChunk(size_t chunk) const {
  auto cs = chunks();
  JIT_THROW_IF(
      chunk >= cs.size(),
      "BitVector::getBitChunk() with chunk {} but vector has {} chunks",
      chunk,
      cs.size());
  return cs[chunk];
}

void BitVector::setBitChunk(size_t chunk, uint64_t bits) {
  auto cs = chunks();
  auto num_chunks = cs.size();
  JIT_THROW_IF(
      chunk >= cs.size(),
      "BitVector::setBitChunk() with chunk {} but vector has {} chunks",
      chunk,
      num_chunks);

  if (chunk == num_chunks - 1) {
    auto remainder = num_bits_ % kChunkBitWidth;
    if (remainder != 0) {
      auto mask = ~((uint64_t{1} << remainder) - 1);
      JIT_THROW_IF(
          (mask & bits) != 0,
          "BitVector::setBitChunk() on final chunk {} but value {} is too big "
          "for vector bitsize {}",
          chunk,
          bits,
          num_bits_);
    }
  }

  cs[chunk] = bits;
}

size_t BitVector::addBits(size_t i) {
  auto new_num_bits = num_bits_ + i;
  setBitWidth(new_num_bits);
  return new_num_bits;
}

void BitVector::setBitWidth(size_t size) {
  if (num_bits_ == size) {
    return;
  }

  bool old_short = isShortVector();
  auto new_num_bits = size;
  num_bits_ = new_num_bits;
  bool new_short = isShortVector();

  if (old_short && !new_short) {
    size_t size_2 = chunksForBits(num_bits_);
    auto old_bits = bits;
    bit_vec = new std::vector<uint64_t>(size_2);
    bit_vec->at(0) = old_bits;
  } else if (!old_short && !new_short) {
    size_t size_2 = chunksForBits(num_bits_);
    bit_vec->resize(size_2);
  } else if (!old_short && new_short) {
    auto low_bits = bit_vec->at(0);
    delete bit_vec;
    bits = low_bits;
  }

  // Clear the unused upper bits of the last chunk.  Could use the BZHI
  // instruction, but this function is not frequently called, so it is okay.
  if (auto remainder = num_bits_ % kChunkBitWidth; remainder != 0) {
    auto high_mask = (uint64_t{1} << remainder) - 1;
    if (new_short) {
      bits &= high_mask;
    } else {
      auto& chunk = bit_vec->back();
      chunk &= high_mask;
    }
  }
}

size_t BitVector::getNumBits() const {
  return num_bits_;
}

size_t BitVector::getPopCount() const {
  size_t count = 0;
  for (uint64_t chunk : chunks()) {
    count += std::popcount(chunk);
  }
  return count;
}

bool BitVector::isEmpty() const {
  return std::ranges::all_of(
      chunks(), [](uint64_t chunk) { return chunk == 0; });
}

std::span<uint64_t> BitVector::chunks() {
  return isShortVector() ? std::span{&bits, chunksForBits(num_bits_)}
                         : std::span{bit_vec->data(), bit_vec->size()};
}

std::span<const uint64_t> BitVector::chunks() const {
  auto mutable_span = const_cast<BitVector*>(this)->chunks();
  return {mutable_span.begin(), mutable_span.size()};
}

bool BitVector::isShortVector() const {
  return num_bits_ <= kShortBitWidth;
}

std::ostream& operator<<(std::ostream& os, const BitVector& bv) {
  os << '[';
  for (std::size_t i = 0, n = bv.getNumBits(); i < n; ++i) {
    if (i > 0 && (i % 8) == 0) {
      os << ';';
    }
    os << (bv.getBit(i) ? '1' : '0');
  }
  os << ']';
  return os;
}

} // namespace cinderx::jit::util
