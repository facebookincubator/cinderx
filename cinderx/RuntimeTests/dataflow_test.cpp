// Copyright (c) Meta Platforms, Inc. and affiliates.
#include <gtest/gtest.h>

#include "cinderx/Jit/dataflow.h"

using namespace cinderx::jit::optimizer;

// This test runs the example found in Section 8.1 of
// the book Advanced Compiler Design And Implementation
TEST(DataFlowTest, ReachingTest) {
  DataFlowAnalyzer<std::string> analyzer;
  auto objects = std::to_array<std::string>(
      {"m:1", "f0:2", "f1:3", "i:5", "f2:8", "f0:9", "f1:10", "i:11"});
  for (const std::string& obj : objects) {
    analyzer.addObject(obj);
  }

  DataFlowBlock& ENTRY = analyzer.createBlock();
  DataFlowBlock& EXIT = analyzer.createBlock();
  DataFlowBlock& b1 = analyzer.createBlock();
  DataFlowBlock& b2 = analyzer.createBlock();
  DataFlowBlock& b3 = analyzer.createBlock();
  DataFlowBlock& b4 = analyzer.createBlock();
  DataFlowBlock& b5 = analyzer.createBlock();
  DataFlowBlock& b6 = analyzer.createBlock();

  ENTRY.connectTo(b1);
  b1.connectTo(b2);
  b1.connectTo(b3);
  b2.connectTo(EXIT);
  b3.connectTo(b4);
  b4.connectTo(b5);
  b4.connectTo(b6);
  b5.connectTo(EXIT);
  b6.connectTo(b4);

  analyzer.setBlockGenBit(b1, "m:1");
  analyzer.setBlockGenBit(b1, "f0:2");
  analyzer.setBlockGenBit(b1, "f1:3");

  analyzer.setBlockKillBit(b1, "f0:9");
  analyzer.setBlockKillBit(b1, "f1:10");

  analyzer.setBlockGenBit(b3, "i:5");
  analyzer.setBlockKillBit(b3, "i:11");

  analyzer.setBlockGenBit(b6, "f2:8");
  analyzer.setBlockGenBit(b6, "f0:9");
  analyzer.setBlockGenBit(b6, "f1:10");
  analyzer.setBlockGenBit(b6, "i:11");

  analyzer.setBlockKillBit(b6, "f0:2");
  analyzer.setBlockKillBit(b6, "f1:3");
  analyzer.setBlockKillBit(b6, "i:5");

  analyzer.solve(Direction::Forward);

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
