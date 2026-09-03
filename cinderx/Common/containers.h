// Copyright (c) Meta Platforms, Inc. and affiliates.
/*
 * Type-aliases for map and set containers. This acts as a shim to allow
 * swapping between STL, phmap, and one day other container implementations.
 *
 * While phmap types can be used as an exact drop-in for STL, it includes more
 * optimized containers that can be used if you do not need pointer-stability.
 * That is, pointers to container content will be invalidated on container
 * mutation. To this end the "base" types here (Set, Map, OrderedSet,
 * OrderedMap) do not provide pointer-stability. If you need this, use the
 * StablePointer variants where available.
 *
 * Additionally the StablePointer* variants may have better performance if the
 * contained values are more than 100 bytes or so. This arises as they will not
 * be moving so much data around when rebalancing etc.
 *
 * The phmap Big* variants may have better performance if the number of
 * contained elements is very large. I haven't played with them but apparently
 * they might take advantage of multi-threading?
 */

#pragma once

// #define JIT_FORCE_STL_CONTAINERS

#ifdef JIT_FORCE_STL_CONTAINERS
#include <map>
#include <set>
#include <unordered_map>
#include <unordered_set>
#else
#include <parallel_hashmap/btree.h>
#include <parallel_hashmap/phmap.h>
#endif

namespace cinderx {

#ifdef JIT_FORCE_STL_CONTAINERS

template <typename... Args>
using UnorderedSet = std::unordered_set<Args...>;
template <typename... Args>
using UnorderedMap = std::unordered_map<Args...>;
template <typename... Args>
using UnorderedStablePointerSet = std::unordered_set<Args...>;
template <typename... Args>
using UnorderedStablePointerMap = std::unordered_map<Args...>;
template <typename... Args>
using UnorderedBigSet = std::unordered_set<Args...>;
template <typename... Args>
using UnorderedBigMap = std::unordered_map<Args...>;
template <typename... Args>
using UnorderedBigStablePointerSet = std::unordered_set<Args...>;
template <typename... Args>
using UnorderedBigStablePointerMap = std::unordered_map<Args...>;
template <typename... Args>
using OrderedSet = std::set<Args...>;
template <typename... Args>
using OrderedMap = std::map<Args...>;
template <typename... Args>
using OrderedMultiset = std::multiset<Args...>;
template <typename... Args>
using OrderedMultimap = std::multimap<Args...>;

#else // JIT_FORCE_STL_CONTAINERS

template <typename... Args>
using UnorderedSet = phmap::flat_hash_set<Args...>;
template <typename... Args>
using UnorderedMap = phmap::flat_hash_map<Args...>;
template <typename... Args>
using UnorderedStablePointerSet = phmap::node_hash_set<Args...>;
template <typename... Args>
using UnorderedStablePointerMap = phmap::node_hash_map<Args...>;
template <typename... Args>
using UnorderedBigSet = phmap::parallel_flat_hash_set<Args...>;
template <typename... Args>
using UnorderedBigMap = phmap::parallel_flat_hash_map<Args...>;
template <typename... Args>
using UnorderedBigStablePointerSet = phmap::parallel_node_hash_set<Args...>;
template <typename... Args>
using UnorderedBigStablePointerMap = phmap::parallel_node_hash_map<Args...>;
template <typename... Args>
using OrderedSet = phmap::btree_set<Args...>;
template <typename... Args>
using OrderedMap = phmap::btree_map<Args...>;
template <typename... Args>
using OrderedMultiset = phmap::btree_multiset<Args...>;
template <typename... Args>
using OrderedMultimap = phmap::btree_multimap<Args...>;

#endif

} // namespace cinderx
