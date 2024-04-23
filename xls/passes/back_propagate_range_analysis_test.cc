// Copyright 2024 The XLS Authors
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//      http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "xls/passes/back_propagate_range_analysis.h"

#include <string_view>

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/types/span.h"
#include "xls/common/status/matchers.h"
#include "xls/ir/bits.h"
#include "xls/ir/function.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/interval.h"
#include "xls/ir/interval_set.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/op.h"
#include "xls/passes/range_query_engine.h"

namespace m = xls::op_matchers;
namespace xls {
namespace {

using testing::ElementsAre;
using testing::Pair;
using testing::UnorderedElementsAre;

class BackPropagateRangeAnalysisTest : public IrTestBase {
 public:
  auto LiteralPair(const Bits& value) {
    return Pair(m::Literal(value), IntervalSet::Precise(value));
  }
};

// Super basic check that we can call this without issues.
TEST_F(BackPropagateRangeAnalysisTest, PropagateNothing) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto arg = fb.Param("arg", p->GetBitsType(4));
  // There is nothing that can be gained from (== 0 (and-reduce X)) since all it
  // means is at least one bit is 0.
  auto target = fb.AndReduce(arg);

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results, PropagateOneGivenBackwards(qe, target.node(), UBits(0, 1)));
  EXPECT_THAT(results, ElementsAre(Pair(target.node(),
                                        IntervalSet::Precise(UBits(0, 1)))));
}

TEST_F(BackPropagateRangeAnalysisTest, SignedLessThanX) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto arg = fb.Param("arg", p->GetBitsType(4));
  auto target = fb.SLt(arg, fb.Literal(UBits(2, 4)));

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results, PropagateOneGivenBackwards(qe, target.node(), UBits(1, 1)));
  EXPECT_THAT(
      results,
      UnorderedElementsAre(
          LiteralPair(UBits(2, 4)),
          Pair(target.node(), IntervalSet::Precise(UBits(1, 1))),
          Pair(arg.node(),
               IntervalSet::Of(
                   {Interval::Closed(UBits(0, 4), UBits(1, 4)),
                    Interval::Closed(Bits::MinSigned(4), SBits(-1, 4))}))));
}

TEST_F(BackPropagateRangeAnalysisTest, LessThanX) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto arg = fb.Param("arg", p->GetBitsType(4));
  auto target = fb.ULt(arg, fb.Literal(UBits(2, 4)));

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results, PropagateOneGivenBackwards(qe, target.node(), UBits(1, 1)));
  EXPECT_THAT(
      results,
      UnorderedElementsAre(
          LiteralPair(UBits(2, 4)),
          Pair(target.node(), IntervalSet::Precise(UBits(1, 1))),
          Pair(arg.node(),
               IntervalSet::Of({Interval::Closed(UBits(0, 4), UBits(1, 4))}))));
}

TEST_F(BackPropagateRangeAnalysisTest, Between) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto arg = fb.Param("arg", p->GetBitsType(4));
  auto target = fb.And(fb.UGt(arg, fb.Literal(UBits(0, 4))),
                       fb.ULt(arg, fb.Literal(UBits(5, 4))));

  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results, PropagateOneGivenBackwards(qe, target.node(), UBits(1, 1)));
  EXPECT_THAT(
      results,
      UnorderedElementsAre(
          LiteralPair(UBits(0, 4)), LiteralPair(UBits(5, 4)),
          Pair(target.node(), IntervalSet::Precise(UBits(1, 1))),
          Pair(arg.node(),
               IntervalSet::Of({Interval::Closed(UBits(1, 4), UBits(4, 4))})),
          Pair(target.node()->operand(0), IntervalSet::Precise(UBits(1, 1))),
          Pair(target.node()->operand(1), IntervalSet::Precise(UBits(1, 1)))));
}


TEST_F(BackPropagateRangeAnalysisTest, MultipleGivens) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto param = fb.Param("foo", p->GetBitsType(8));
  auto secret_limit = fb.Param("secret_limit", p->GetBitsType(8));
  auto compare = fb.ULe(param, secret_limit);
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results,
      PropagateGivensBackwards(
          qe, f,
          {{compare.node(), IntervalSet::Precise(UBits(1, 1))},
           {secret_limit.node(), IntervalSet::Precise(UBits(32, 8))}}));

  EXPECT_THAT(
      results,
      UnorderedElementsAre(
          Pair(secret_limit.node(), IntervalSet::Precise(UBits(32, 8))),
          Pair(compare.node(), IntervalSet::Precise(UBits(1, 1))),
          Pair(param.node(),
               IntervalSet::Of({Interval(UBits(0, 8), UBits(32, 8))}))));
}

TEST_F(BackPropagateRangeAnalysisTest, And) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto a1 = fb.Param("a1", p->GetBitsType(1));
  auto a2 = fb.Param("a2", p->GetBitsType(1));
  auto a3 = fb.Param("a3", p->GetBitsType(1));
  auto a4 = fb.Param("a4", p->GetBitsType(1));
  auto a5 = fb.Param("a5", p->GetBitsType(1));
  auto a6 = fb.Param("a6", p->GetBitsType(1));
  auto comp = fb.And({a1, a2, a3, a4, a5, a6});
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_true,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(1, 1))}}));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_false,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(0, 1))}}));

  EXPECT_THAT(
      results_true,
      UnorderedElementsAre(Pair(comp.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a1.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a2.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a3.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a4.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a5.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a6.node(), IntervalSet::Precise(UBits(1, 1)))));
  EXPECT_THAT(results_false,
              UnorderedElementsAre(
                  Pair(comp.node(), IntervalSet::Precise(UBits(0, 1)))));
}
TEST_F(BackPropagateRangeAnalysisTest, Or) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto a1 = fb.Param("a1", p->GetBitsType(1));
  auto a2 = fb.Param("a2", p->GetBitsType(1));
  auto a3 = fb.Param("a3", p->GetBitsType(1));
  auto a4 = fb.Param("a4", p->GetBitsType(1));
  auto a5 = fb.Param("a5", p->GetBitsType(1));
  auto a6 = fb.Param("a6", p->GetBitsType(1));
  auto comp = fb.Or({a1, a2, a3, a4, a5, a6});
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_true,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(1, 1))}}));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_false,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(0, 1))}}));

  EXPECT_THAT(
      results_false,
      UnorderedElementsAre(Pair(comp.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a1.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a2.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a3.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a4.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a5.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a6.node(), IntervalSet::Precise(UBits(0, 1)))));
  EXPECT_THAT(results_true,
              UnorderedElementsAre(
                  Pair(comp.node(), IntervalSet::Precise(UBits(1, 1)))));
}

TEST_F(BackPropagateRangeAnalysisTest, Nand) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto a1 = fb.Param("a1", p->GetBitsType(1));
  auto a2 = fb.Param("a2", p->GetBitsType(1));
  auto a3 = fb.Param("a3", p->GetBitsType(1));
  auto a4 = fb.Param("a4", p->GetBitsType(1));
  auto a5 = fb.Param("a5", p->GetBitsType(1));
  auto a6 = fb.Param("a6", p->GetBitsType(1));
  auto comp = fb.AddNaryOp(Op::kNand, {a1, a2, a3, a4, a5, a6});
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_true,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(1, 1))}}));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_false,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(0, 1))}}));

  EXPECT_THAT(
      results_false,
      UnorderedElementsAre(Pair(comp.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a1.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a2.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a3.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a4.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a5.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a6.node(), IntervalSet::Precise(UBits(1, 1)))));
  EXPECT_THAT(results_true,
              UnorderedElementsAre(
                  Pair(comp.node(), IntervalSet::Precise(UBits(1, 1)))));
}

TEST_F(BackPropagateRangeAnalysisTest, Nor) {
  auto p = CreatePackage();
  FunctionBuilder fb(TestName(), p.get());
  auto a1 = fb.Param("a1", p->GetBitsType(1));
  auto a2 = fb.Param("a2", p->GetBitsType(1));
  auto a3 = fb.Param("a3", p->GetBitsType(1));
  auto a4 = fb.Param("a4", p->GetBitsType(1));
  auto a5 = fb.Param("a5", p->GetBitsType(1));
  auto a6 = fb.Param("a6", p->GetBitsType(1));
  auto comp = fb.AddNaryOp(Op::kNor, {a1, a2, a3, a4, a5, a6});
  XLS_ASSERT_OK_AND_ASSIGN(Function * f, fb.Build());

  RangeQueryEngine qe;
  XLS_ASSERT_OK(qe.Populate(f).status());
  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_true,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(1, 1))}}));

  XLS_ASSERT_OK_AND_ASSIGN(
      auto results_false,
      PropagateGivensBackwards(
          qe, f, {{comp.node(), IntervalSet::Precise(UBits(0, 1))}}));

  EXPECT_THAT(
      results_true,
      UnorderedElementsAre(Pair(comp.node(), IntervalSet::Precise(UBits(1, 1))),
                           Pair(a1.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a2.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a3.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a4.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a5.node(), IntervalSet::Precise(UBits(0, 1))),
                           Pair(a6.node(), IntervalSet::Precise(UBits(0, 1)))));
  EXPECT_THAT(results_false,
              UnorderedElementsAre(
                  Pair(comp.node(), IntervalSet::Precise(UBits(0, 1)))));
}

}  // namespace
}  // namespace xls
