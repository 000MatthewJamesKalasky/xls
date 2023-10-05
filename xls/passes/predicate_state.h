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

#ifndef XLS_PASSES_PREDICATE_STATE_H_
#define XLS_PASSES_PREDICATE_STATE_H_

#include <cstddef>
#include <cstdint>
#include <variant>

#include "absl/strings/str_format.h"
#include "absl/types/variant.h"
#include "xls/common/logging/logging.h"
#include "xls/common/visitor.h"
#include "xls/ir/node.h"
#include "xls/ir/nodes.h"

namespace xls {

// Special value denoting the 'default' arm.
struct DefaultArm : public std::monostate {
  template <typename H>
  friend H AbslHashValue(H h, const DefaultArm& a) {
    return H::combine(std::move(h), std::monostate{});
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const DefaultArm& arm) {
    absl::Format(&sink, "DEFAULT");
  }
};

// Abstraction representing a the state of some select operation.
class PredicateState {
 public:
  using ArmT = std::variant<int64_t, DefaultArm>;
  static constexpr ArmT kDefaultArm{DefaultArm{}};
  using SelectT =
      std::variant<Select*, OneHotSelect*, PrioritySelect*, std::nullptr_t>;
  PredicateState() : node_(nullptr), index_(kDefaultArm) {}
  PredicateState(SelectT node, ArmT index) : node_(node), index_(index) {}
  PredicateState(const PredicateState&) = default;
  PredicateState& operator=(const PredicateState&) = default;
  PredicateState(PredicateState&&) = default;
  PredicateState& operator=(PredicateState&&) = default;

  // Does this state represent no selects guarding.
  bool IsBasePredicate() const {
    return std::holds_alternative<std::nullptr_t>(node_);
  }

  // Is the arm the 'default' arm (assuming that's even meaningful for the
  // select).
  bool IsDefaultArm() const { return kDefaultArm == index_; }

  // The select this predicate represents as a node.
  Node* node() const {
    return absl::visit([](auto v) -> Node* { return v; }, node_);
  }

  // The value which controls the select
  Node* selector() const {
    XLS_CHECK(!IsBasePredicate());
    // All have selector as op(0)
    return node()->operand(0);
  }

  Node* value() const {
    XLS_CHECK(!IsBasePredicate());
    return absl::visit(xls::Visitor{[&](Select* s) -> Node* {
                                      return IsDefaultArm()
                                                 ? s->default_value().value()
                                                 : s->get_case(arm_index());
                                    },
                                    [&](OneHotSelect* s) -> Node* {
                                      XLS_CHECK(!IsDefaultArm());
                                      return s->get_case(arm_index());
                                    },
                                    [&](PrioritySelect* s) -> Node* {
                                      XLS_CHECK(!IsDefaultArm());
                                      return s->get_case(arm_index());
                                    },
                                    [](std::nullptr_t) -> Node* {
                                      XLS_LOG(FATAL) << "Unreachable";
                                      return nullptr;
                                    }},
                       node_);
  }

  // The arm index this predicate protects
  ArmT arm() const { return index_; }
  int64_t arm_index() const {
    XLS_CHECK(std::holds_alternative<int64_t>(index_));
    return std::get<int64_t>(index_);
  }

  friend bool operator==(const PredicateState& x, const PredicateState& y) {
    return (x.node_ == y.node_) && (x.index_ == y.index_);
  }

  template <typename H>
  friend H AbslHashValue(H h, const PredicateState& ps) {
    return H::combine(std::move(h), ps.node_, ps.index_);
  }

  template <typename Sink>
  friend void AbslStringify(Sink& sink, const PredicateState& state) {
    if (state.IsBasePredicate()) {
      absl::Format(&sink, "PredicateState[Base]");
    } else {
      absl::visit(
          [&](auto index) {
            absl::Format(&sink, "PredicateState[%v: arm: %v]", *state.node(),
                         index);
          },
          state.index_);
    }
  }

 private:
  SelectT node_;
  ArmT index_;
};
}  // namespace xls

#endif  // XLS_PASSES_PREDICATE_STATE_H_
