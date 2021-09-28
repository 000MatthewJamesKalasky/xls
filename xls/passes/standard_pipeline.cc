// Copyright 2020 The XLS Authors
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

// Create a standard pipeline of passes. This pipeline should
// be used in the main driver as well as in testing.

#include "xls/passes/standard_pipeline.h"

#include "absl/status/statusor.h"
#include "xls/passes/arith_simplification_pass.h"
#include "xls/passes/array_simplification_pass.h"
#include "xls/passes/bdd_cse_pass.h"
#include "xls/passes/bdd_simplification_pass.h"
#include "xls/passes/bit_slice_simplification_pass.h"
#include "xls/passes/boolean_simplification_pass.h"
#include "xls/passes/canonicalization_pass.h"
#include "xls/passes/concat_simplification_pass.h"
#include "xls/passes/constant_folding_pass.h"
#include "xls/passes/cse_pass.h"
#include "xls/passes/dce_pass.h"
#include "xls/passes/dfe_pass.h"
#include "xls/passes/identity_removal_pass.h"
#include "xls/passes/inlining_pass.h"
#include "xls/passes/literal_uncommoning_pass.h"
#include "xls/passes/map_inlining_pass.h"
#include "xls/passes/narrowing_pass.h"
#include "xls/passes/reassociation_pass.h"
#include "xls/passes/select_simplification_pass.h"
#include "xls/passes/strength_reduction_pass.h"
#include "xls/passes/table_switch_pass.h"
#include "xls/passes/tuple_simplification_pass.h"
#include "xls/passes/unroll_pass.h"
#include "xls/passes/verifier_checker.h"

namespace xls {

class SimplificationPass : public FixedPointCompoundPass {
 public:
  explicit SimplificationPass(int64_t opt_level)
      : FixedPointCompoundPass("simp", "Simplification") {
    Add<ConstantFoldingPass>();
    Add<DeadCodeEliminationPass>();
    Add<CanonicalizationPass>();
    Add<DeadCodeEliminationPass>();
    Add<ArithSimplificationPass>(opt_level);
    Add<DeadCodeEliminationPass>();
    Add<TableSwitchPass>();
    Add<DeadCodeEliminationPass>();
    Add<SelectSimplificationPass>(opt_level);
    Add<DeadCodeEliminationPass>();
    Add<ReassociationPass>();
    Add<DeadCodeEliminationPass>();
    Add<ConstantFoldingPass>();
    Add<DeadCodeEliminationPass>();
    Add<BitSliceSimplificationPass>(opt_level);
    Add<DeadCodeEliminationPass>();
    Add<ConcatSimplificationPass>(opt_level);
    Add<DeadCodeEliminationPass>();
    Add<TupleSimplificationPass>();
    Add<DeadCodeEliminationPass>();
    Add<StrengthReductionPass>(opt_level);
    Add<DeadCodeEliminationPass>();
    Add<ArraySimplificationPass>(opt_level);
    Add<DeadCodeEliminationPass>();
    Add<NarrowingPass>(opt_level);
    Add<DeadCodeEliminationPass>();
    Add<BooleanSimplificationPass>();
    Add<DeadCodeEliminationPass>();
    Add<CsePass>();
  }
};

std::unique_ptr<CompoundPass> CreateStandardPassPipeline(int64_t opt_level) {
  auto top = std::make_unique<CompoundPass>("ir", "Top level pass pipeline");
  top->AddInvariantChecker<VerifierChecker>();

  top->Add<DeadFunctionEliminationPass>();
  top->Add<DeadCodeEliminationPass>();
  top->Add<IdentityRemovalPass>();
  // At this stage in the pipeline only optimizations up to level 2 should
  // run. 'opt_level' is the maximum level of optimization which should be run
  // in the entire pipeline so set the level of the simplification pass to the
  // minimum of the two values. Same below.
  top->Add<SimplificationPass>(std::min(int64_t{2}, opt_level));
  top->Add<UnrollPass>();
  top->Add<MapInliningPass>();
  top->Add<InliningPass>();
  top->Add<DeadFunctionEliminationPass>();
  top->Add<BddSimplificationPass>(std::min(int64_t{2}, opt_level));
  top->Add<DeadCodeEliminationPass>();
  top->Add<BddCsePass>();
  top->Add<DeadCodeEliminationPass>();
  top->Add<SimplificationPass>(std::min(int64_t{2}, opt_level));

  top->Add<BddSimplificationPass>(std::min(int64_t{3}, opt_level));
  top->Add<DeadCodeEliminationPass>();
  top->Add<BddCsePass>();
  top->Add<DeadCodeEliminationPass>();
  top->Add<SimplificationPass>(std::min(int64_t{3}, opt_level));
  top->Add<LiteralUncommoningPass>();
  top->Add<DeadFunctionEliminationPass>();
  return top;
}

absl::StatusOr<bool> RunStandardPassPipeline(Package* package,
                                             int64_t opt_level) {
  std::unique_ptr<CompoundPass> pipeline = CreateStandardPassPipeline();
  PassResults results;
  return pipeline->Run(package, PassOptions(), &results);
}

}  // namespace xls
