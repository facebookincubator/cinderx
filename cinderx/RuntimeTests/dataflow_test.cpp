// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/dataflow.h"

using namespace cinderx::jit::optimizer;

// This test runs the example found in Section 8.1 of
// the book Advanced Compiler Design And Implementation
TEST(DataFlowTest, ReachingTest) {
  DataFlowAnalyzer<std::string> analyzer;
  analyzer.addObjects(
      {"m:1", "f0:2", "f1:3", "i:5", "f2:8", "f0:9", "f1:10", "i:11"});

  DataFlowBlock b1, b2, b3, b4, b5, b6, ENTRY, EXIT;

  ENTRY.connectTo(b1);
  b1.connectTo(b2);
  b1.connectTo(b3);
  b2.connectTo(EXIT);
  b3.connectTo(b4);
  b4.connectTo(b5);
  b4.connectTo(b6);
  b5.connectTo(EXIT);
  b6.connectTo(b4);

  analyzer.addBlock(ENTRY);
  analyzer.addBlock(EXIT);
  analyzer.addBlock(b1);
  analyzer.addBlock(b2);
  analyzer.addBlock(b3);
  analyzer.addBlock(b4);
  analyzer.addBlock(b5);
  analyzer.addBlock(b6);

  analyzer.setBlockGenBits(b1, {"m:1", "f0:2", "f1:3"});
  analyzer.setBlockKillBits(b1, {"f0:9", "f1:10"});

  analyzer.setBlockGenBits(b3, {"i:5"});
  analyzer.setBlockKillBits(b3, {"i:11"});

  analyzer.setBlockGenBits(b6, {"f2:8", "f0:9", "f1:10", "i:11"});
  analyzer.setBlockKillBits(b6, {"f0:2", "f1:3", "i:5"});

  analyzer.setEntryBlock(ENTRY);
  analyzer.setEntryBlock(EXIT);

  analyzer.runAnalysis();

  ASSERT_EQ(ENTRY.in_.getBitChunk(), 0);
  ASSERT_EQ(b1.in_.getBitChunk(), 0);
  ASSERT_EQ(b2.in_.getBitChunk(), 7);
  ASSERT_EQ(b3.in_.getBitChunk(), 7);
  ASSERT_EQ(b4.in_.getBitChunk(), 0xff);
  ASSERT_EQ(b5.in_.getBitChunk(), 0xff);
  ASSERT_EQ(b6.in_.getBitChunk(), 0xff);
  ASSERT_EQ(EXIT.in_.getBitChunk(), 0xff);

  ASSERT_EQ(ENTRY.out_.getBitChunk(), 0);
  ASSERT_EQ(b1.out_.getBitChunk(), 7);
  ASSERT_EQ(b2.out_.getBitChunk(), 7);
  ASSERT_EQ(b3.out_.getBitChunk(), 0xf);
  ASSERT_EQ(b4.out_.getBitChunk(), 0xff);
  ASSERT_EQ(b5.out_.getBitChunk(), 0xff);
  ASSERT_EQ(b6.out_.getBitChunk(), 0xf1);
  ASSERT_EQ(EXIT.out_.getBitChunk(), 0xff);
}
