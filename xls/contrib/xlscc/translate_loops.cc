// Copyright 2022 The XLS Authors
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

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "clang/include/clang/AST/Decl.h"
#include "clang/include/clang/AST/Expr.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/status_macros.h"
#include "xls/contrib/xlscc/cc_parser.h"
#include "xls/contrib/xlscc/translator.h"
#include "xls/contrib/xlscc/xlscc_logging.h"
#include "xls/ir/bits.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/source_location.h"
#include "xls/ir/type.h"

using std::shared_ptr;
using std::string;
using std::vector;

namespace {

// Returns monotonically increasing time in seconds
double doubletime() {
  struct timeval tv;
  struct timezone tz;
  gettimeofday(&tv, &tz);
  return tv.tv_sec + static_cast<double>(tv.tv_usec) / 1000000.0;
}

}  // namespace

namespace xlscc {

absl::Status Translator::GenerateIR_Loop(
    bool always_first_iter, const clang::Stmt* init,
    const clang::Expr* cond_expr, const clang::Stmt* inc,
    const clang::Stmt* body, const clang::PresumedLoc& presumed_loc,
    const xls::SourceInfo& loc, clang::ASTContext& ctx) {
  if (cond_expr != nullptr && cond_expr->isIntegerConstantExpr(ctx)) {
    // special case for "for (;0;) {}" (essentially no op)
    XLS_ASSIGN_OR_RETURN(auto constVal, EvaluateInt64(*cond_expr, ctx, loc));
    if (constVal == 0) {
      return absl::OkStatus();
    }
  }

  bool have_relevant_intrinsic = false;
  bool intrinsic_unroll = false;

  XLS_ASSIGN_OR_RETURN(const clang::CallExpr* intrinsic_call,
                       FindIntrinsicCall(presumed_loc));
  if (intrinsic_call != nullptr) {
    const std::string& intrinsic_name =
        intrinsic_call->getDirectCallee()->getNameAsString();

    if (intrinsic_name == "__xlscc_pipeline") {
      have_relevant_intrinsic = true;
      intrinsic_unroll = false;
    } else if (intrinsic_name == "__xlscc_unroll") {
      have_relevant_intrinsic = true;
      intrinsic_unroll = true;
    }
  }

  XLS_ASSIGN_OR_RETURN(Pragma pragma, FindPragmaForLoc(presumed_loc));

  bool have_relevant_pragma =
      (pragma.type() == Pragma_Unroll || pragma.type() == Pragma_InitInterval);

  if (have_relevant_intrinsic && have_relevant_pragma) {
    return absl::InvalidArgumentError(
        ErrorMessage(loc,
                     "Have both an __xlscc_ intrinsic and a #pragma directive, "
                     "don't know what to do"));
  }

  bool do_unroll = false;

  if ((have_relevant_intrinsic && intrinsic_unroll) ||
      (pragma.type() == Pragma_Unroll) || context().for_loops_default_unroll) {
    do_unroll = true;
  }

  if (do_unroll) {
    return GenerateIR_UnrolledLoop(always_first_iter, init, cond_expr, inc,
                                   body, ctx, loc);
  }

  int64_t init_interval = -1;

  if (have_relevant_intrinsic) {
    XLSCC_CHECK(!intrinsic_unroll, loc);
    XLSCC_CHECK_EQ(intrinsic_call->getNumArgs(), 1, loc);
    XLS_ASSIGN_OR_RETURN(init_interval,
                         EvaluateInt64(*intrinsic_call->getArg(0), ctx, loc));
  } else if (have_relevant_pragma) {
    XLSCC_CHECK(pragma.type() == Pragma_InitInterval, loc);
    init_interval = pragma.int_argument();
  }

  if (have_relevant_intrinsic || have_relevant_pragma) {
    if (init_interval <= 0) {
      return absl::InvalidArgumentError(
          ErrorMessage(loc, "Invalid initiation interval %i", init_interval));
    }
  }

  // Pipelined loops can inherit their initiation interval from enclosing
  // loops, so they can be allowed not to have a #pragma.
  if (init_interval < 0) {
    XLS_CHECK(!context().in_pipelined_for_body ||
              (context().outer_pipelined_loop_init_interval > 0));
    init_interval = context().outer_pipelined_loop_init_interval;
  }
  if (init_interval <= 0) {
    return absl::UnimplementedError(
        ErrorMessage(loc, "For loop missing #pragma or __xlscc_ intrinsic"));
  }

  // Pipelined do-while
  return GenerateIR_PipelinedLoop(always_first_iter, init, cond_expr, inc, body,
                                  init_interval, ctx, loc);
}

absl::Status Translator::GenerateIR_UnrolledLoop(bool always_first_iter,
                                                 const clang::Stmt* init,
                                                 const clang::Expr* cond_expr,
                                                 const clang::Stmt* inc,
                                                 const clang::Stmt* body,
                                                 clang::ASTContext& ctx,
                                                 const xls::SourceInfo& loc) {
  XLS_ASSIGN_OR_RETURN(
      std::unique_ptr<xls::solvers::z3::IrTranslator> z3_translator_parent,
      xls::solvers::z3::IrTranslator::CreateAndTranslate(
          /*source=*/nullptr,
          /*allow_unsupported=*/false));

  Z3_solver solver =
      xls::solvers::z3::CreateSolver(z3_translator_parent->ctx(), 1);

  class SolverDeref {
   public:
    SolverDeref(Z3_context ctx, Z3_solver solver)
        : ctx_(ctx), solver_(solver) {}
    ~SolverDeref() { Z3_solver_dec_ref(ctx_, solver_); }

   private:
    Z3_context ctx_;
    Z3_solver solver_;
  };

  // Generate the declaration within a private context
  PushContextGuard for_init_guard(*this, loc);
  context().propagate_break_up = false;
  context().propagate_continue_up = false;
  context().in_for_body = true;
  context().in_switch_body = false;

  if (init != nullptr) {
    XLS_RETURN_IF_ERROR(GenerateIR_Stmt(init, ctx));
  }

  // Loop unrolling causes duplicate NamedDecls which fail the soundness
  // check. Reset the known set before each iteration.
  auto saved_check_ids = unique_decl_ids_;

  double slowest_iter = 0;

  for (int64_t nIters = 0;; ++nIters) {
    const bool first_iter = nIters == 0;
    const bool always_this_iter = always_first_iter && first_iter;

    const double iter_start = doubletime();

    unique_decl_ids_ = saved_check_ids;

    if (nIters > max_unroll_iters_) {
      return absl::ResourceExhaustedError(
          ErrorMessage(loc, "Loop unrolling broke at maximum %i iterations",
                       max_unroll_iters_));
    }
    if (nIters == warn_unroll_iters_) {
      XLS_LOG(WARNING) << ErrorMessage(
          loc, "Loop unrolling has reached %i iterations", warn_unroll_iters_);
    }

    // Generate condition.
    //
    // Outside of body context guard so it applies to increment
    // Also, if this is inside the body context guard then the break condition
    // feeds back on itself in an explosion of complexity
    // via assignments to any variables used in the condition.
    if (!always_this_iter && cond_expr != nullptr) {
      XLS_ASSIGN_OR_RETURN(CValue cond_expr_cval,
                           GenerateIR_Expr(cond_expr, loc));
      XLS_CHECK(cond_expr_cval.type()->Is<CBoolType>());
      context().or_condition_util(
          context().fb->Not(cond_expr_cval.rvalue(), loc),
          context().relative_break_condition, loc);
      XLS_RETURN_IF_ERROR(and_condition(cond_expr_cval.rvalue(), loc));
    }

    {
      // We use the relative condition so that returns also stop unrolling
      XLS_ASSIGN_OR_RETURN(bool condition_must_be_false,
                           BitMustBe(false, context().relative_condition,
                                     solver, z3_translator_parent->ctx(), loc));
      if (condition_must_be_false) {
        break;
      }
    }

    // Generate body
    {
      PushContextGuard for_body_guard(*this, loc);
      context().propagate_break_up = true;
      context().propagate_continue_up = false;

      XLS_RETURN_IF_ERROR(GenerateIR_Compound(body, ctx));
    }

    // Generate increment
    // Outside of body guard because continue would skip.
    if (inc != nullptr) {
      XLS_RETURN_IF_ERROR(GenerateIR_Stmt(inc, ctx));
    }
    // Print slow unrolling warning
    const double iter_end = doubletime();
    const double iter_seconds = iter_end - iter_start;

    if (iter_seconds > 0.1 && iter_seconds > slowest_iter) {
      XLS_LOG(WARNING) << ErrorMessage(loc,
                                       "Slow loop unrolling iteration %i: %fms",
                                       nIters, iter_seconds * 1000.0);
      slowest_iter = iter_seconds;
    }
  }

  return absl::OkStatus();
}

bool Translator::LValueContainsOnlyChannels(std::shared_ptr<LValue> lvalue) {
  if (lvalue == nullptr) {
    return true;
  }

  if (lvalue->get_compounds().empty() && lvalue->channel_leaf() == nullptr) {
    return false;
  }

  for (const auto& [idx, lval_field] : lvalue->get_compounds()) {
    if (!LValueContainsOnlyChannels(lval_field)) {
      return false;
    }
  }

  return true;
}

// Must match order in TranslateLValueConditions

absl::Status Translator::SendLValueConditions(
    const std::shared_ptr<LValue>& lvalue,
    std::vector<xls::BValue>* lvalue_conditions, const xls::SourceInfo& loc) {
  for (const auto& [idx, compound_lval] : lvalue->get_compounds()) {
    XLS_RETURN_IF_ERROR(
        SendLValueConditions(compound_lval, lvalue_conditions, loc));
  }
  if (!lvalue->is_select()) {
    return absl::OkStatus();
  }
  lvalue_conditions->push_back(lvalue->cond());

  XLS_RETURN_IF_ERROR(
      SendLValueConditions(lvalue->lvalue_true(), lvalue_conditions, loc));
  XLS_RETURN_IF_ERROR(
      SendLValueConditions(lvalue->lvalue_false(), lvalue_conditions, loc));

  return absl::OkStatus();
}

// Must match order in SendLValueConditions
absl::StatusOr<std::shared_ptr<LValue>> Translator::TranslateLValueConditions(
    const std::shared_ptr<LValue>& outer_lvalue,
    xls::BValue lvalue_conditions_tuple, const xls::SourceInfo& loc,
    int64_t* at_index) {
  if (outer_lvalue == nullptr) {
    return nullptr;
  }
  if (!outer_lvalue->get_compounds().empty()) {
    absl::flat_hash_map<int64_t, std::shared_ptr<LValue>> compounds;
    for (const auto& [idx, compound_lval] : outer_lvalue->get_compounds()) {
      XLS_ASSIGN_OR_RETURN(
          compounds[idx],
          TranslateLValueConditions(compound_lval, lvalue_conditions_tuple, loc,
                                    at_index));
    }
    return std::make_shared<LValue>(compounds);
  }

  if (!outer_lvalue->is_select()) {
    return outer_lvalue;
  }
  int64_t at_index_storage = 0;
  if (at_index == nullptr) {
    at_index = &at_index_storage;
  }
  xls::BValue translated_condition =
      context().fb->TupleIndex(lvalue_conditions_tuple, *at_index, loc);
  ++(*at_index);

  XLS_ASSIGN_OR_RETURN(
      std::shared_ptr<LValue> translated_lvalue_true,
      TranslateLValueConditions(outer_lvalue->lvalue_true(),
                                lvalue_conditions_tuple, loc, at_index));
  XLS_ASSIGN_OR_RETURN(
      std::shared_ptr<LValue> translated_lvalue_false,
      TranslateLValueConditions(outer_lvalue->lvalue_false(),
                                lvalue_conditions_tuple, loc, at_index));

  return std::make_shared<LValue>(translated_condition, translated_lvalue_true,
                                  translated_lvalue_false);
}

absl::Status Translator::GenerateIR_PipelinedLoop(
    bool always_first_iter, const clang::Stmt* init,
    const clang::Expr* cond_expr, const clang::Stmt* inc,
    const clang::Stmt* body, int64_t initiation_interval_arg,
    clang::ASTContext& ctx, const xls::SourceInfo& loc) {
  XLS_RETURN_IF_ERROR(CheckInitIntervalValidity(initiation_interval_arg, loc));

  // Generate the loop counter declaration within a private context
  // By doing this here, it automatically gets rolled into proc state
  // This causes it to be automatically reset on break
  PushContextGuard for_init_guard(*this, loc);

  if (init != nullptr) {
    XLS_RETURN_IF_ERROR(GenerateIR_Stmt(init, ctx));
  }

  // Condition must be checked at the start
  if (!always_first_iter && cond_expr != nullptr) {
    XLS_ASSIGN_OR_RETURN(CValue cond_cval, GenerateIR_Expr(cond_expr, loc));
    XLS_CHECK(cond_cval.type()->Is<CBoolType>());

    XLS_RETURN_IF_ERROR(and_condition(cond_cval.rvalue(), loc));
  }

  // Pack context tuple
  std::shared_ptr<CStructType> context_cvars_struct_ctype;
  std::shared_ptr<CInternalTuple> context_lval_conds_ctype;

  xls::Type* context_struct_xls_type = nullptr;
  xls::Type* context_lval_xls_type = nullptr;
  CValue context_tuple_out;
  absl::flat_hash_map<const clang::NamedDecl*, uint64_t> variable_field_indices;
  std::vector<const clang::NamedDecl*> variable_fields_order;
  {
    std::vector<std::shared_ptr<CField>> fields;
    std::vector<xls::BValue> tuple_values;

    XLS_ASSIGN_OR_RETURN(const clang::VarDecl* on_reset_var_decl,
                         parser_->GetXlsccOnReset());

    // Create a deterministic field order
    for (const auto& [decl, _] : context().variables) {
      XLS_CHECK(context().sf->declaration_order_by_name_.contains(decl));
      // Don't pass __xlscc_on_reset in/out
      if (decl == on_reset_var_decl) {
        continue;
      }
      variable_fields_order.push_back(decl);
    }

    context().sf->SortNamesDeterministically(variable_fields_order);

    std::vector<xls::BValue> lvalue_conditions;

    for (const clang::NamedDecl* decl : variable_fields_order) {
      const CValue& cvalue = context().variables.at(decl);

      if (cvalue.rvalue().valid()) {
        const uint64_t field_idx = tuple_values.size();
        variable_field_indices[decl] = field_idx;
        tuple_values.push_back(cvalue.rvalue());
        auto field_ptr =
            std::make_shared<CField>(decl, field_idx, cvalue.type());
        fields.push_back(field_ptr);
      }

      if (cvalue.lvalue() != nullptr) {
        XLS_RETURN_IF_ERROR(
            SendLValueConditions(cvalue.lvalue(), &lvalue_conditions, loc));
      }
    }

    xls::BValue lvalue_conditions_tuple;
    lvalue_conditions_tuple = context().fb->Tuple(lvalue_conditions, loc);
    std::vector<std::shared_ptr<CType>> lvalue_conds_tuple_fields;
    lvalue_conds_tuple_fields.resize(lvalue_conditions.size(),
                                     std::make_shared<CBoolType>());
    context_lval_conds_ctype =
        std::make_shared<CInternalTuple>(lvalue_conds_tuple_fields);

    context_cvars_struct_ctype = std::make_shared<CStructType>(
        fields, /*no_tuple=*/false, /*synthetic_int=*/false);
    CValue context_struct_out =
        CValue(MakeStructXLS(tuple_values, *context_cvars_struct_ctype, loc),
               context_cvars_struct_ctype);

    std::vector<std::shared_ptr<CType>> context_tuple_elem_types;
    context_tuple_elem_types.push_back(context_cvars_struct_ctype);
    context_tuple_elem_types.push_back(context_lval_conds_ctype);
    std::shared_ptr<CInternalTuple> context_tuple_type =
        std::make_shared<CInternalTuple>(context_tuple_elem_types);

    // Set later if needed
    xls::BValue outer_on_reset_value =
        context().fb->Literal(xls::UBits(0, 1), loc);

    // Must match if(uses_on_reset) below
    context_tuple_out = CValue(
        context().fb->Tuple({outer_on_reset_value, context_struct_out.rvalue(),
                             lvalue_conditions_tuple}),
        context_tuple_type);

    context_struct_xls_type = context_struct_out.rvalue().GetType();
    context_lval_xls_type = lvalue_conditions_tuple.GetType();
  }

  // Create synthetic channels and IO ops
  xls::Type* context_xls_type = context_tuple_out.rvalue().GetType();

  const std::string name_prefix =
      absl::StrFormat("__for_%i", next_for_number_++);

  IOChannel* context_out_channel = nullptr;
  {
    std::string ch_name = absl::StrFormat("%s_ctx_out", name_prefix);
    XLS_ASSIGN_OR_RETURN(
        xls::Channel * xls_channel,
        package_->CreateStreamingChannel(
            ch_name, xls::ChannelOps::kSendReceive, context_xls_type,
            /*initial_values=*/{}, /*fifo_depth=*/0,
            xls::FlowControl::kReadyValid));
    IOChannel new_channel;
    new_channel.item_type = context_tuple_out.type();
    new_channel.unique_name = ch_name;
    new_channel.generated = xls_channel;
    context_out_channel = AddChannel(new_channel, loc);
  }
  IOChannel* context_in_channel = nullptr;
  {
    std::string ch_name = absl::StrFormat("%s_ctx_in", name_prefix);
    XLS_ASSIGN_OR_RETURN(
        xls::Channel * xls_channel,
        package_->CreateStreamingChannel(
            ch_name, xls::ChannelOps::kSendReceive, context_struct_xls_type,
            /*initial_values=*/{}, /*fifo_depth=*/0,
            xls::FlowControl::kReadyValid));
    IOChannel new_channel;
    new_channel.item_type = context_cvars_struct_ctype;
    new_channel.unique_name = ch_name;
    new_channel.generated = xls_channel;
    context_in_channel = AddChannel(new_channel, loc);
  }

  // Create loop body proc
  absl::flat_hash_map<const clang::NamedDecl*, std::shared_ptr<LValue>>
      lvalues_out;
  bool uses_on_reset = false;
  std::vector<const clang::NamedDecl*> vars_changed_in_body;
  XLS_ASSIGN_OR_RETURN(
      PipelinedLoopSubProc sub_proc,
      GenerateIR_PipelinedLoopBody(
          cond_expr, inc, body, initiation_interval_arg, ctx, name_prefix,
          context_out_channel, context_in_channel, context_struct_xls_type,
          context_lval_xls_type, context_cvars_struct_ctype,
          context_lval_conds_ctype, &lvalues_out, variable_field_indices,
          variable_fields_order, vars_changed_in_body, &uses_on_reset, loc));

  // Record sub-proc for generation later
  context().sf->sub_procs.push_back(std::move(sub_proc));

  XLS_CHECK_EQ(vars_changed_in_body.size(), lvalues_out.size());

  if (uses_on_reset) {
    XLS_ASSIGN_OR_RETURN(CValue on_reset_cval, GetOnReset(loc));
    XLSCC_CHECK_EQ(on_reset_cval.type()->GetBitWidth(), 1, loc);

    // Must match tuple creation above
    context_tuple_out = CValue(
        context().fb->Tuple(
            {on_reset_cval.rvalue(),
             context().fb->TupleIndex(context_tuple_out.rvalue(), 1, loc),
             context().fb->TupleIndex(context_tuple_out.rvalue(), 2, loc)}),
        context_tuple_out.type());
  }

  // Send and receive context tuples
  IOOp* ctx_out_op_ptr = nullptr;
  {
    IOOp op;
    op.op = OpType::kSend;
    std::vector<xls::BValue> sp = {context_tuple_out.rvalue(),
                                   context().full_condition_bval(loc)};
    op.ret_value = context().fb->Tuple(sp, loc);
    XLS_ASSIGN_OR_RETURN(ctx_out_op_ptr,
                         AddOpToChannel(op, context_out_channel, loc));
  }

  IOOp* ctx_in_op_ptr;
  {
    IOOp op;
    op.op = OpType::kRecv;
    op.ret_value = context().full_condition_bval(loc);
    XLS_ASSIGN_OR_RETURN(ctx_in_op_ptr,
                         AddOpToChannel(op, context_in_channel, loc));
  }

  ctx_in_op_ptr->after_ops.push_back(ctx_out_op_ptr);

  // Unpack context tuple
  xls::BValue context_tuple_recvd = ctx_in_op_ptr->input_value.rvalue();
  {
    // Don't assign to variables that aren't changed in the loop body,
    // as this creates extra state
    for (const clang::NamedDecl* decl : vars_changed_in_body) {
      if (!variable_field_indices.contains(decl)) {
        continue;
      }

      const uint64_t field_idx = variable_field_indices.at(decl);

      const CValue prev_cval = context().variables.at(decl);

      const CValue cval(GetStructFieldXLS(context_tuple_recvd, field_idx,
                                          *context_cvars_struct_ctype, loc),
                        prev_cval.type(), /*disable_type_check=*/false,
                        lvalues_out.at(decl));
      XLS_RETURN_IF_ERROR(Assign(decl, cval, loc));
    }
  }

  return absl::OkStatus();
}

absl::StatusOr<PipelinedLoopSubProc> Translator::GenerateIR_PipelinedLoopBody(
    const clang::Expr* cond_expr, const clang::Stmt* inc,
    const clang::Stmt* body, int64_t init_interval, clang::ASTContext& ctx,
    std::string_view name_prefix, IOChannel* context_out_channel,
    IOChannel* context_in_channel, xls::Type* context_struct_xls_type,
    xls::Type* context_lvals_xls_type,
    const std::shared_ptr<CStructType>& context_cvars_struct_ctype,
    const std::shared_ptr<CInternalTuple>& context_lval_conds_ctype,
    absl::flat_hash_map<const clang::NamedDecl*, std::shared_ptr<LValue>>*
        lvalues_out,
    const absl::flat_hash_map<const clang::NamedDecl*, uint64_t>&
        variable_field_indices,
    const std::vector<const clang::NamedDecl*>& variable_fields_order,
    std::vector<const clang::NamedDecl*>& vars_changed_in_body,
    bool* uses_on_reset, const xls::SourceInfo& loc) {
  const uint64_t total_context_values =
      context_cvars_struct_ctype->fields().size();
  std::vector<const clang::NamedDecl*> vars_to_save_between_iters;

  GeneratedFunction& enclosing_func = *context().sf;

  // Generate body function
  auto generated_func = std::make_unique<GeneratedFunction>();
  XLS_CHECK_NE(context().sf, nullptr);
  XLS_CHECK_NE(context().sf->clang_decl, nullptr);
  generated_func->clang_decl = context().sf->clang_decl;
  uint64_t extra_return_count = 0;
  {
    // Set up IR generation
    xls::FunctionBuilder body_builder(absl::StrFormat("%s_func", name_prefix),
                                      package_);

    xls::BValue context_struct_val =
        body_builder.Param(absl::StrFormat("%s_context_vars", name_prefix),
                           context_struct_xls_type, loc);
    xls::BValue context_lvalues_val =
        body_builder.Param(absl::StrFormat("%s_context_lvals", name_prefix),
                           context_lvals_xls_type, loc);
    xls::BValue context_on_reset_val =
        body_builder.Param(absl::StrFormat("%s_on_reset", name_prefix),
                           package_->GetBitsType(1), loc);

    TranslationContext& prev_context = context();
    PushContextGuard context_guard(*this, loc);

    context() = TranslationContext();
    context().propagate_up = false;

    context().fb = absl::implicit_cast<xls::BuilderBase*>(&body_builder);
    context().sf = generated_func.get();
    context().ast_context = prev_context.ast_context;
    context().in_pipelined_for_body = true;
    context().outer_pipelined_loop_init_interval = init_interval;

    absl::flat_hash_map<IOChannel*, IOChannel*> inner_channels_by_outer_channel;
    absl::flat_hash_map<IOChannel*, IOChannel*> outer_channels_by_inner_channel;

    // Inherit external channels
    for (IOChannel& enclosing_channel : enclosing_func.io_channels) {
      if (enclosing_channel.generated != nullptr) {
        continue;
      }
      generated_func->io_channels.push_back(enclosing_channel);
      IOChannel* inner_channel = &generated_func->io_channels.back();
      inner_channel->total_ops = 0;

      inner_channels_by_outer_channel[&enclosing_channel] = inner_channel;
      outer_channels_by_inner_channel[inner_channel] = &enclosing_channel;

      XLSCC_CHECK(
          external_channels_by_internal_channel_.contains(&enclosing_channel),
          loc);

      if (external_channels_by_internal_channel_.count(&enclosing_channel) >
          1) {
        return absl::UnimplementedError(
            ErrorMessage(loc,
                         "IO ops in pipelined loops in subroutines called "
                         "with multiple different channel arguments"));
      }

      const ChannelBundle enclosing_bundle =
          external_channels_by_internal_channel_.find(&enclosing_channel)
              ->second;

      // Don't use = .at(), avoid compiler bug
      std::pair<const IOChannel*, ChannelBundle> pair(inner_channel,
                                                      enclosing_bundle);
      if (!ContainsKeyValuePair(external_channels_by_internal_channel_, pair)) {
        external_channels_by_internal_channel_.insert(pair);
      }
    }

    // Declare __xlscc_on_reset
    XLS_ASSIGN_OR_RETURN(const clang::VarDecl* on_reset_var_decl,
                         parser_->GetXlsccOnReset());
    XLS_RETURN_IF_ERROR(DeclareVariable(
        on_reset_var_decl,
        CValue(context_on_reset_val, std::make_shared<CBoolType>()), loc,
        /*check_unique_ids=*/false));

    // Context in
    absl::flat_hash_map<const clang::NamedDecl*, CValue> prev_vars;

    for (const clang::NamedDecl* decl : variable_fields_order) {
      const CValue& outer_value = prev_context.variables.at(decl);
      xls::BValue param_bval;
      if (variable_field_indices.contains(decl)) {
        const uint64_t field_idx = variable_field_indices.at(decl);
        param_bval =
            GetStructFieldXLS(context_struct_val, static_cast<int>(field_idx),
                              *context_cvars_struct_ctype, loc);
      }

      std::shared_ptr<LValue> inner_lval;
      XLS_ASSIGN_OR_RETURN(
          inner_lval,
          TranslateLValueChannels(outer_value.lvalue(),
                                  inner_channels_by_outer_channel, loc));

      XLS_ASSIGN_OR_RETURN(
          inner_lval,
          TranslateLValueConditions(inner_lval, context_lvalues_val, loc));

      CValue prev_var(param_bval, outer_value.type(),
                      /*disable_type_check=*/false, inner_lval);
      prev_vars[decl] = prev_var;

      // __xlscc_on_reset handled separately
      if (decl == on_reset_var_decl) {
        continue;
      }

      XLS_RETURN_IF_ERROR(
          DeclareVariable(decl, prev_var, loc, /*check_unique_ids=*/false));
    }

    xls::BValue do_break = context().fb->Literal(xls::UBits(0, 1));

    // Generate body
    // Don't apply continue conditions to increment
    // This context pop will top generate selects
    {
      PushContextGuard context_guard(*this, loc);
      context().propagate_break_up = false;
      context().propagate_continue_up = false;
      context().in_for_body = true;

      XLS_CHECK_GT(context().outer_pipelined_loop_init_interval, 0);

      XLS_CHECK_NE(body, nullptr);
      XLS_RETURN_IF_ERROR(GenerateIR_Compound(body, ctx));

      // break_condition is the assignment condition
      if (context().relative_break_condition.valid()) {
        xls::BValue break_cond = context().relative_break_condition;
        do_break = context().fb->Or(do_break, break_cond, loc);
      }
    }

    // Increment
    // Break condition skips increment
    if (inc != nullptr) {
      // This context pop will top generate selects
      PushContextGuard context_guard(*this, loc);
      XLS_RETURN_IF_ERROR(and_condition(context().fb->Not(do_break, loc), loc));
      XLS_RETURN_IF_ERROR(GenerateIR_Stmt(inc, ctx));
    }

    // Check condition
    if (cond_expr != nullptr) {
      // This context pop will top generate selects
      PushContextGuard context_guard(*this, loc);

      XLS_ASSIGN_OR_RETURN(CValue cond_cval, GenerateIR_Expr(cond_expr, loc));
      XLS_CHECK(cond_cval.type()->Is<CBoolType>());
      xls::BValue break_on_cond_val = context().fb->Not(cond_cval.rvalue());

      do_break = context().fb->Or(do_break, break_on_cond_val, loc);
    }

    // Context out
    std::vector<xls::BValue> tuple_values;
    tuple_values.resize(total_context_values);
    for (const clang::NamedDecl* decl : variable_fields_order) {
      if (!variable_field_indices.contains(decl)) {
        continue;
      }
      const uint64_t field_idx = variable_field_indices.at(decl);
      tuple_values[field_idx] = context().variables.at(decl).rvalue();
    }

    xls::BValue ret_ctx =
        MakeStructXLS(tuple_values, *context_cvars_struct_ctype, loc);
    std::vector<xls::BValue> return_bvals = {ret_ctx, do_break};

    // For GenerateIRBlock_Prepare() / GenerateIOInvokes()
    extra_return_count += return_bvals.size();

    // First static returns
    for (const clang::NamedDecl* decl :
         generated_func->GetDeterministicallyOrderedStaticValues()) {
      XLS_ASSIGN_OR_RETURN(CValue value, GetIdentifier(decl, loc));
      return_bvals.push_back(value.rvalue());
    }

    // IO returns
    for (IOOp& op : generated_func->io_ops) {
      XLS_CHECK(op.ret_value.valid());
      return_bvals.push_back(op.ret_value);
    }

    xls::BValue ret_val = MakeFlexTuple(return_bvals, loc);
    generated_func->return_value_count = return_bvals.size();
    XLS_ASSIGN_OR_RETURN(generated_func->xls_func,
                         body_builder.BuildWithReturnValue(ret_val));

    // Analyze context variables changed
    for (const clang::NamedDecl* decl : variable_fields_order) {
      const CValue prev_bval = prev_vars.at(decl);
      const CValue curr_val = context().variables.at(decl);
      if (prev_bval.rvalue().node() != curr_val.rvalue().node() ||
          prev_bval.lvalue() != curr_val.lvalue()) {
        vars_changed_in_body.push_back(decl);
        XLS_ASSIGN_OR_RETURN(
            (*lvalues_out)[decl],
            TranslateLValueChannels(curr_val.lvalue(),
                                    outer_channels_by_inner_channel, loc));
      }
    }

    context().sf->SortNamesDeterministically(vars_changed_in_body);

    // All variables now are saved in state, because a streaming channel
    // is used for the context
    vars_to_save_between_iters = variable_fields_order;
  }

  XLSCC_CHECK_NE(uses_on_reset, nullptr, loc);
  if (generated_func->uses_on_reset) {
    *uses_on_reset = true;
  }

  PipelinedLoopSubProc pipelined_loop_proc = {
      .name_prefix = name_prefix.data(),
      .context_out_channel = context_out_channel,
      .context_in_channel = context_in_channel,
      .context_cvars_struct_ctype = context_cvars_struct_ctype,
      .context_lval_conds_ctype = context_lval_conds_ctype,
      .loc = loc,

      .vars_to_save_between_iters = vars_to_save_between_iters,
      .enclosing_func = context().sf,
      .outer_variables = context().variables,
      .variable_field_indices = variable_field_indices,
      .total_context_values = total_context_values,
      .extra_return_count = extra_return_count,
      .generated_func = std::move(generated_func)};

  // TODO(seanhaskell): Move this to GenerateIR_Block() for pipelined loops
  // with multiple different sets of IO ops
  XLS_RETURN_IF_ERROR(GenerateIR_PipelinedLoopProc(pipelined_loop_proc));

  return pipelined_loop_proc;
}

absl::Status Translator::GenerateIR_PipelinedLoopProc(
    const PipelinedLoopSubProc& pipelined_loop_proc) {
  const std::string& name_prefix = pipelined_loop_proc.name_prefix;
  IOChannel* context_out_channel = pipelined_loop_proc.context_out_channel;
  IOChannel* context_in_channel = pipelined_loop_proc.context_in_channel;
  const std::shared_ptr<CStructType>& context_cvars_struct_ctype =
      pipelined_loop_proc.context_cvars_struct_ctype;
  const std::shared_ptr<CInternalTuple>& context_lval_conds_ctype =
      pipelined_loop_proc.context_lval_conds_ctype;
  const xls::SourceInfo& loc = pipelined_loop_proc.loc;

  const std::vector<const clang::NamedDecl*>& vars_to_save_between_iters =
      pipelined_loop_proc.vars_to_save_between_iters;
  const absl::flat_hash_map<const clang::NamedDecl*, uint64_t>&
      variable_field_indices = pipelined_loop_proc.variable_field_indices;

  const uint64_t total_context_values =
      pipelined_loop_proc.total_context_values;
  const uint64_t extra_return_count = pipelined_loop_proc.extra_return_count;
  const GeneratedFunction& generated_func = *pipelined_loop_proc.generated_func;

  // Generate body proc
  xls::ProcBuilder pb(absl::StrFormat("%s_proc", name_prefix),
                      /*token_name=*/"tkn", package_);

  int64_t extra_state_count = 0;

  // Construct initial state
  pb.StateElement("__first_tick", xls::Value(xls::UBits(1, 1)));
  ++extra_state_count;
  XLS_ASSIGN_OR_RETURN(xls::Value default_lval_conds,
                       CreateDefaultRawValue(context_lval_conds_ctype, loc));
  pb.StateElement("__lvalue_conditions", default_lval_conds);
  ++extra_state_count;

  const int64_t builtin_state_count = extra_state_count;

  for (const clang::NamedDecl* decl : vars_to_save_between_iters) {
    if (!variable_field_indices.contains(decl)) {
      continue;
    }
    const CValue& prev_value = pipelined_loop_proc.outer_variables.at(decl);
    XLS_ASSIGN_OR_RETURN(xls::Value def, CreateDefaultRawValue(
                                             prev_value.type(), GetLoc(*decl)));
    pb.StateElement(decl->getNameAsString(), def);
    ++extra_state_count;
  }

  // For utility functions like MakeStructXls()
  PushContextGuard pb_guard(*this, loc);
  context().fb = absl::implicit_cast<xls::BuilderBase*>(&pb);

  xls::BValue token = pb.GetTokenParam();

  xls::BValue first_iter_state_in = pb.GetStateParam(0);

  xls::BValue recv_condition = first_iter_state_in;
  XLS_CHECK_EQ(recv_condition.GetType()->GetFlatBitCount(), 1);

  xls::BValue receive =
      pb.ReceiveIf(context_out_channel->generated, token, recv_condition, loc);
  xls::BValue token_ctx = pb.TupleIndex(receive, 0);
  xls::BValue received_context_tuple = pb.TupleIndex(receive, 1);

  xls::BValue received_on_reset = pb.TupleIndex(received_context_tuple, 0, loc);
  xls::BValue received_context = pb.TupleIndex(received_context_tuple, 1, loc);
  xls::BValue received_lvalue_conds =
      pb.TupleIndex(received_context_tuple, 2, loc);

  xls::BValue lvalue_conditions_tuple = context().fb->Select(
      first_iter_state_in, received_lvalue_conds, pb.GetStateParam(1), loc);

  // Deal with on_reset
  xls::BValue on_reset_bval;

  if (generated_func.uses_on_reset) {
    // received_on_reset is only valid in the first iteration, but that's okay
    // as & first_iter_state_in will always be 0 in subsequent iterations.
    on_reset_bval = pb.And(first_iter_state_in, received_on_reset, loc);
  } else {
    on_reset_bval = pb.Literal(xls::UBits(0, 1), loc);
  }

  token = token_ctx;

  // Add selects for changed context variables
  xls::BValue selected_context;
  {
    std::vector<xls::BValue> context_values;
    for (uint64_t fi = 0; fi < total_context_values; ++fi) {
      context_values.push_back(GetStructFieldXLS(
          received_context, fi, *context_cvars_struct_ctype, loc));
    }

    // After first flag
    uint64_t state_tup_idx = builtin_state_count;
    for (const clang::NamedDecl* decl : vars_to_save_between_iters) {
      if (!variable_field_indices.contains(decl)) {
        continue;
      }
      const uint64_t field_idx = variable_field_indices.at(decl);
      XLS_CHECK_LT(field_idx, context_values.size());
      xls::BValue context_val = GetStructFieldXLS(
          received_context, field_idx, *context_cvars_struct_ctype, loc);
      xls::BValue prev_state_val = pb.GetStateParam(state_tup_idx++);

      context_values[field_idx] =
          pb.Select(first_iter_state_in, context_val, prev_state_val, loc);
    }
    selected_context =
        MakeStructXLS(context_values, *context_cvars_struct_ctype, loc);
  }

  for (const IOOp& op : generated_func.io_ops) {
    if (op.op == OpType::kTrace) {
      continue;
    }
    if (op.channel->generated != nullptr) {
      continue;
    }
    XLS_CHECK(io_test_mode_ ||
              external_channels_by_internal_channel_.contains(op.channel));
  }

  // Invoke loop over IOs
  PreparedBlock prepared;
  prepared.xls_func = &generated_func;
  prepared.args.push_back(selected_context);
  prepared.args.push_back(lvalue_conditions_tuple);
  prepared.args.push_back(on_reset_bval);
  prepared.token = token;

  XLS_RETURN_IF_ERROR(
      GenerateIRBlockPrepare(prepared, pb,
                             /*next_return_index=*/extra_return_count,
                             /*next_state_index=*/extra_state_count,
                             /*this_type=*/nullptr,
                             /*this_decl=*/nullptr,
                             /*top_decls=*/{}, loc));

  XLS_ASSIGN_OR_RETURN(xls::BValue ret_tup,
                       GenerateIOInvokes(prepared, pb, loc));

  token = prepared.token;

  xls::BValue updated_context = pb.TupleIndex(ret_tup, 0, loc);
  xls::BValue do_break = pb.TupleIndex(ret_tup, 1, loc);

  // Send back context on break
  token = pb.SendIf(context_in_channel->generated, token, do_break,
                    updated_context, loc);

  // Construct next state
  std::vector<xls::BValue> next_state_values = {
      // First iteration next tick?
      do_break, lvalue_conditions_tuple};
  XLSCC_CHECK_EQ(next_state_values.size(), builtin_state_count, loc);
  for (const clang::NamedDecl* decl : vars_to_save_between_iters) {
    if (!variable_field_indices.contains(decl)) {
      continue;
    }
    const uint64_t field_idx = variable_field_indices.at(decl);
    xls::BValue val = GetStructFieldXLS(updated_context, field_idx,
                                        *context_cvars_struct_ctype, loc);
    next_state_values.push_back(val);
  }
  for (const clang::NamedDecl* namedecl :
       prepared.xls_func->GetDeterministicallyOrderedStaticValues()) {
    XLS_CHECK(context().fb == &pb);

    next_state_values.push_back(pb.TupleIndex(
        ret_tup, prepared.return_index_for_static.at(namedecl), loc));
  }

  //  xls::BValue next_state = pb.Tuple(next_state_values);
  XLS_RETURN_IF_ERROR(pb.Build(token, next_state_values).status());

  return absl::OkStatus();
}

}  // namespace xlscc
