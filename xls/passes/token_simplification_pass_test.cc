// Copyright 2023 The XLS Authors
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

#include "xls/passes/token_simplification_pass.h"

#include "gmock/gmock.h"
#include "gtest/gtest.h"
#include "absl/status/statusor.h"
#include "xls/common/status/matchers.h"
#include "xls/common/status/status_macros.h"
#include "xls/ir/function.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_matcher.h"
#include "xls/ir/ir_test_base.h"
#include "xls/ir/package.h"

namespace m = ::xls::op_matchers;

namespace xls {
namespace {

using status_testing::IsOkAndHolds;

class TokenSimplificationPassTest : public IrTestBase {
 protected:
  TokenSimplificationPassTest() = default;

  absl::StatusOr<bool> Run(FunctionBase* f) {
    PassResults results;
    XLS_ASSIGN_OR_RETURN(bool changed,
                         TokenSimplificationPass().RunOnFunctionBase(
                             f, PassOptions(), &results));
    return changed;
  }
};

TEST_F(TokenSimplificationPassTest, SingleArgument) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> p, ParsePackage(R"(
     package test_module

     top proc main(tok: token, state: (), init={()}) {
       after_all.1: token = after_all(tok)
       tuple.2: () = tuple()
       next (after_all.1, tuple.2)
     }
  )"));
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, p->GetTopAsProc());
  EXPECT_THAT(Run(proc), IsOkAndHolds(true));
  EXPECT_THAT(proc->NextToken(), proc->TokenParam());
}

TEST_F(TokenSimplificationPassTest, DuplicatedArgument) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> p, ParsePackage(R"(
     package test_module

     top proc main(tok: token, state: (), init={()}) {
       after_all.1: token = after_all(tok, tok, tok)
       tuple.2: () = tuple()
       next (after_all.1, tuple.2)
     }
  )"));
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, p->GetTopAsProc());
  EXPECT_THAT(Run(proc), IsOkAndHolds(true));
  EXPECT_THAT(proc->NextToken(), proc->TokenParam());
}

TEST_F(TokenSimplificationPassTest, NestedAfterAll) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> p, ParsePackage(R"(
     package test_module

     top proc main(tok: token, state: (), init={()}) {
       after_all.1: token = after_all(tok, tok, tok)
       after_all.2: token = after_all(after_all.1, tok, tok)
       tuple.3: () = tuple()
       next (after_all.2, tuple.3)
     }
  )"));
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, p->GetTopAsProc());
  EXPECT_THAT(Run(proc), IsOkAndHolds(true));
  EXPECT_THAT(proc->NextToken(), proc->TokenParam());
}

TEST_F(TokenSimplificationPassTest, DuplicatedArgument2) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> p, ParsePackage(R"(
     package test_module

     chan test_channel(
       bits[32], id=0, kind=streaming, ops=send_only,
       flow_control=ready_valid, metadata="""""")

     top proc main(tok: token, state: (), init={()}) {
       literal.1: bits[32] = literal(value=10)
       send.2: token = send(tok, literal.1, channel_id=0)
       send.3: token = send(send.2, literal.1, channel_id=0)
       send.4: token = send(tok, literal.1, channel_id=0)
       after_all.5: token = after_all(send.2, send.3, send.4)
       tuple.6: () = tuple()
       next (after_all.5, tuple.6)
     }
  )"));
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, p->GetTopAsProc());
  EXPECT_THAT(Run(proc), IsOkAndHolds(true));
  EXPECT_THAT(proc->NextToken(),
              m::AfterAll(m::Send(m::Send(proc->TokenParam(), m::Literal()),
                                  m::Literal()),
                          m::Send(proc->TokenParam(), m::Literal())));
}

TEST_F(TokenSimplificationPassTest, UnrelatedArguments) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> p, ParsePackage(R"(
     package test_module

     chan test_channel(
       bits[32], id=0, kind=streaming, ops=send_only,
       flow_control=ready_valid, metadata="""""")

     top proc main(tok: token, state: (), init={()}) {
       literal.1: bits[32] = literal(value=10)
       send.2: token = send(tok, literal.1, channel_id=0)
       send.3: token = send(tok, literal.1, channel_id=0)
       send.4: token = send(tok, literal.1, channel_id=0)
       after_all.5: token = after_all(send.2, send.3, send.4)
       tuple.6: () = tuple()
       next (after_all.5, tuple.6)
     }
  )"));
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, p->GetTopAsProc());
  EXPECT_THAT(Run(proc), IsOkAndHolds(false));
  EXPECT_THAT(proc->NextToken(),
              m::AfterAll(m::Send(proc->TokenParam(), m::Literal()),
                          m::Send(proc->TokenParam(), m::Literal()),
                          m::Send(proc->TokenParam(), m::Literal())));
}

TEST_F(TokenSimplificationPassTest, ArgumentsWithDependencies) {
  XLS_ASSERT_OK_AND_ASSIGN(std::unique_ptr<Package> p, ParsePackage(R"(
     package test_module

     chan test_channel(
       bits[32], id=0, kind=streaming, ops=send_only,
       flow_control=ready_valid, metadata="""""")

     top proc main(tok: token, state: (), init={()}) {
       literal.1: bits[32] = literal(value=10)
       send.2: token = send(tok, literal.1, channel_id=0)
       send.3: token = send(send.2, literal.1, channel_id=0)
       after_all.4: token = after_all(tok, send.2, send.3)
       tuple.5: () = tuple()
       next (after_all.4, tuple.5)
     }
  )"));
  XLS_ASSERT_OK_AND_ASSIGN(Proc * proc, p->GetTopAsProc());
  EXPECT_THAT(Run(proc), IsOkAndHolds(true));
  EXPECT_THAT(proc->NextToken(), m::Send());
}

}  // namespace
}  // namespace xls
