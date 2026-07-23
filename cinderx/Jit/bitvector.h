// Copyright (c) Meta Platforms, Inc. and affiliates.

#pragma once

#include <fmt/ostream.h>

#include <concepts>
#include <cstddef>
#include <cstdint>
#include <iosfwd>
#include <span>
#include <vector>

namespace cinderx::jit::util {

class BitVector {
 public:
  BitVector() = default;
  ~BitVector();

  /* implicit */ BitVector(size_t num_bits);

  template <std::integral T>
  BitVector(size_t num_bits, T bits) : BitVector{num_bits} {
    setShortBits(bits);
  }

  BitVector(const BitVector& bv);
  BitVector(BitVector&& bv) noexcept;

  BitVector& operator=(const BitVector& rhs);
  BitVector& operator=(BitVector&& rhs) noexcept;

  // Operators for the bit vector. Due to the purpose of this class (used in DFG
  // analysis), we only support operations between two bit vectors with the same
  // width.
  bool operator==(const BitVector& rhs) const;
  bool operator!=(const BitVector& rhs) const;
  BitVector operator&(const BitVector& rhs) const;
  BitVector operator|(const BitVector& rhs) const;
  BitVector operator-(const BitVector& rhs) const;
  BitVector& operator&=(const BitVector& rhs);
  BitVector& operator|=(const BitVector& rhs);
  BitVector& operator-=(const BitVector& rhs);

  // Reset all bits to 0.
  void resetAll();
  // Set all bits to 1.
  void setAll();

  // Set all bits to `v`.
  void fill(bool v);

  // Get and set a bit in the position specified in bit. The bit index must
  // be in the range of the bit vector.
  bool getBit(size_t bit) const;
  void setBit(size_t bit, bool v = true);

  // Run a function for every set bit, passing it the bit index.
  template <class F>
    requires std::invocable<F, size_t>
  void forEachSetBit(F&& per_bit_func) const {
    size_t chunk_base = 0;
    for (uint64_t chunk : chunks()) {
      while (chunk > 0) {
        int bit = std::countr_zero(chunk);
        chunk ^= chunk & -chunk;
        per_bit_func(bit + chunk_base);
      }
      chunk_base += sizeof(uint64_t) * CHAR_BIT;
    }
  }

  // Get and set the bit vector as a uint64_t.  Only works if its size is small
  // enough to fit in a uint64_t.
  uint64_t shortBits() const;
  void setShortBits(uint64_t bits);

  // Get or set a 64-bit chunk of bits.
  uint64_t getBitChunk(size_t chunk = 0) const;
  void setBitChunk(size_t chunk, uint64_t bits);

  // Add number of bits specified in i to the bit vector.  Returns the new size.
  size_t addBits(size_t i);

  // Resize the bit vector to the number of bits specified in size. If size is
  // less than the current number of bits, the bit vector will be truncated.
  void setBitWidth(size_t size);

  size_t getNumBits() const;
  size_t getPopCount() const;
  bool isEmpty() const;

 private:
  size_t num_bits_{0};

  /*
   * For a bit vector <= 64 bits (which is the bit width of a pointer), the bits
   * are saved inline.  For a larger bit vector, it is divided into 64-bit
   * chunks and saved in a vector.
   */
  union {
    uint64_t bits{0};
    std::vector<uint64_t>* bit_vec;
  };

  std::span<uint64_t> chunks();
  std::span<const uint64_t> chunks() const;

  bool isShortVector() const;

  template <typename Op>
  BitVector binaryOp(const BitVector& rhs, const Op& op) const;

  template <typename Op>
  BitVector& binaryOpAssign(const BitVector& rhs, const Op& op);
};

std::ostream& operator<<(std::ostream& os, const BitVector& bv);

} // namespace cinderx::jit::util

template <>
struct fmt::formatter<cinderx::jit::util::BitVector> : fmt::ostream_formatter {
};
