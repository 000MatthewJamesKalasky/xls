// Copyright 2021 The XLS Authors
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

#include "xls/passes/union_query_engine.h"

#include <memory>
#include <optional>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "xls/common/logging/logging.h"
#include "xls/data_structures/leaf_type_tree.h"
#include "xls/ir/interval_set.h"
#include "xls/ir/node.h"
#include "xls/ir/ternary.h"
#include "xls/ir/type.h"
#include "xls/passes/predicate_state.h"
#include "xls/passes/query_engine.h"

namespace xls {

absl::StatusOr<ReachedFixpoint> UnionQueryEngine::Populate(FunctionBase* f) {
  ReachedFixpoint result = ReachedFixpoint::Unchanged;
  for (const std::unique_ptr<QueryEngine>& engine : engines_) {
    XLS_ASSIGN_OR_RETURN(ReachedFixpoint rf, engine->Populate(f));
    // Unchanged is the top of the lattice so it's an identity
    if (result == ReachedFixpoint::Unchanged) {
      result = rf;
    }
    // Changed can only degrade to Unknown
    if ((result == ReachedFixpoint::Changed) &&
        (rf == ReachedFixpoint::Unknown)) {
      result = ReachedFixpoint::Unknown;
    }
    // No case needed for ReachedFixpoint::Unknown since it's already the bottom
    // of the lattice
  }
  return result;
}

bool UnionQueryEngine::IsTracked(Node* node) const {
  for (const auto& engine : engines_) {
    if (engine->IsTracked(node)) {
      return true;
    }
  }
  return false;
}

LeafTypeTree<TernaryVector> UnionQueryEngine::GetTernary(Node* node) const {
  LeafTypeTree<TernaryVector> result(node->GetType(), [](Type* leaf_type) {
    return TernaryVector(leaf_type->GetFlatBitCount(), TernaryValue::kUnknown);
  });
  for (const auto& engine : engines_) {
    if (engine->IsTracked(node)) {
      LeafTypeTree<TernaryVector> ternary = engine->GetTernary(node);
      result = LeafTypeTree<TernaryVector>::Zip<TernaryVector, TernaryVector>(
          [](const TernaryVector& v1,
             const TernaryVector& v2) -> TernaryVector {
            absl::StatusOr<TernaryVector> r = ternary_ops::Union(v1, v2);
            XLS_CHECK_OK(r);
            return *r;
          },
          result, ternary);
    }
  }
  return result;
}

std::unique_ptr<QueryEngine> UnionQueryEngine::SpecializeGivenPredicate(
    const absl::flat_hash_set<PredicateState>& state) const {
  std::vector<std::unique_ptr<QueryEngine>> engines;
  engines.reserve(engines_.size());
  for (const auto& engine : engines_) {
    engines.push_back(engine->SpecializeGivenPredicate(state));
  }
  return std::make_unique<UnionQueryEngine>(std::move(engines));
}

LeafTypeTree<IntervalSet> UnionQueryEngine::GetIntervals(Node* node) const {
  LeafTypeTree<IntervalSet> result(node->GetType());
  for (int64_t i = 0; i < result.size(); ++i) {
    result.elements()[i] =
        IntervalSet::Maximal(result.leaf_types()[i]->GetFlatBitCount());
  }
  for (const auto& engine : engines_) {
    if (engine->IsTracked(node)) {
      result = LeafTypeTree<IntervalSet>::Zip<IntervalSet, IntervalSet>(
          IntervalSet::Intersect, result, engine->GetIntervals(node));
    }
  }
  return result;
}

bool UnionQueryEngine::AtMostOneTrue(
    absl::Span<TreeBitLocation const> bits) const {
  for (const auto& engine : engines_) {
    if (engine->AtMostOneTrue(bits)) {
      return true;
    }
  }
  return false;
}

bool UnionQueryEngine::AtLeastOneTrue(
    absl::Span<TreeBitLocation const> bits) const {
  for (const auto& engine : engines_) {
    if (engine->AtLeastOneTrue(bits)) {
      return true;
    }
  }
  return false;
}

bool UnionQueryEngine::KnownEquals(const TreeBitLocation& a,
                                   const TreeBitLocation& b) const {
  for (const auto& engine : engines_) {
    if (engine->KnownEquals(a, b)) {
      return true;
    }
  }
  return false;
}

bool UnionQueryEngine::KnownNotEquals(const TreeBitLocation& a,
                                      const TreeBitLocation& b) const {
  for (const auto& engine : engines_) {
    if (engine->KnownNotEquals(a, b)) {
      return true;
    }
  }
  return false;
}

bool UnionQueryEngine::Implies(const TreeBitLocation& a,
                               const TreeBitLocation& b) const {
  for (const auto& engine : engines_) {
    if (engine->Implies(a, b)) {
      return true;
    }
  }
  return false;
}

std::optional<Bits> UnionQueryEngine::ImpliedNodeValue(
    absl::Span<const std::pair<TreeBitLocation, bool>> predicate_bit_values,
    Node* node) const {
  for (const auto& engine : engines_) {
    if (auto i = engine->ImpliedNodeValue(predicate_bit_values, node)) {
      return i;
    }
  }
  return std::nullopt;
}

}  // namespace xls
