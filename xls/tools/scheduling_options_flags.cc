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

#include "xls/tools/scheduling_options_flags.h"

#include <cstdint>
#include <limits>
#include <string>
#include <variant>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/status/status_macros.h"
#include "xls/delay_model/delay_estimator.h"
#include "xls/delay_model/delay_estimators.h"
#include "xls/fdo/synthesizer.h"
#include "xls/ir/package.h"
#include "xls/scheduling/scheduling_options.h"
#include "xls/tools/scheduling_options_flags.pb.h"

// LINT.IfChange
ABSL_FLAG(int64_t, clock_period_ps, 0,
          "Target clock period, in picoseconds. See "
          "https://google.github.io/xls/scheduling for details.");
ABSL_FLAG(int64_t, pipeline_stages, 0,
          "The number of stages in the generated pipeline. See "
          "https://google.github.io/xls/scheduling for details.");
ABSL_FLAG(std::string, delay_model, "",
          "Delay model name to use from registry.");
ABSL_FLAG(int64_t, clock_margin_percent, 0,
          "The percentage of clock period to set aside as a margin to ensure "
          "timing is met. Effectively, this lowers the clock period by this "
          "percentage amount for the purposes of scheduling. See "
          "https://google.github.io/xls/scheduling for details.");
ABSL_FLAG(int64_t, period_relaxation_percent, 0,
          "The percentage of clock period that will be relaxed when "
          "scheduling without an explicit --clock_period_ps. "
          "When set to 0, the minimum period that can satisfy scheduling "
          "constraints will be used. Increasing this will trade-off an "
          "increase in critical path delay in favor of decreased register "
          "count. See https://google.github.io/xls/scheduling for details.");
ABSL_FLAG(
    bool, minimize_clock_on_failure, true,
    "If true, when `--clock_period_ps` is given but is infeasible for "
    "scheduling, search for & report the shortest feasible clock period. "
    "Otherwise, just reports whether increasing the clock period can help.");
ABSL_FLAG(int64_t, worst_case_throughput, 1,
          "Allow scheduling a pipeline with worst-case throughput no slower "
          "than once per N cycles. If unspecified, enforce throughput 1. Note: "
          "a higher value for --worst_case_throughput *decreases* the "
          "worst-case throughput, since this controls inverse throughput.");
ABSL_FLAG(int64_t, additional_input_delay_ps, 0,
          "The additional delay added to each receive node.");
ABSL_FLAG(int64_t, ffi_fallback_delay_ps, 0,
          "Delay of foreign function calls if not otherwise specified.");
ABSL_FLAG(std::vector<std::string>, io_constraints, {},
          "A comma-separated list of IO constraints, each of which is "
          "specified by a literal like `foo:send:bar:recv:3:5` which means "
          "that sends on channel `foo` must occur between 3 and 5 cycles "
          "(inclusive) before receives on channel `bar`. "
          "Note that for a constraint like `foo:send:foo:send:3:5`, no "
          "constraint will be applied between a node and itself; i.e.: this "
          "means all _different_ pairs of nodes sending on `foo` must be in "
          "cycles that differ by between 3 and 5. "
          "If the special minimum/maximum value `none` is used, then "
          "the minimum latency will be the lowest representable int64_t, "
          "and likewise for maximum latency.");
ABSL_FLAG(bool, receives_first_sends_last, false,
          "If true, this forces receives into the first cycle and sends into "
          "the last cycle.");
ABSL_FLAG(int64_t, mutual_exclusion_z3_rlimit, -1,
          "Resource limit for solver in mutual exclusion pass");
ABSL_FLAG(std::string, scheduling_options_proto, "",
          "Path to a protobuf containing all scheduling options args.");
ABSL_FLAG(int64_t, fdo_iteration_number, 1,
          "The number of FDO iterations during the pipeline scheduling. Must "
          "be an integer >= 1.");
ABSL_FLAG(int64_t, fdo_delay_driven_path_number, 0,
          "The number of delay-driven subgraphs in each FDO iteration. Must be "
          "a non-negative integer.");
ABSL_FLAG(int64_t, fdo_fanout_driven_path_number, 0,
          "The number of fanout-driven subgraphs in each FDO iteration. Must "
          "be a non-negative integer.");
ABSL_FLAG(
    float, fdo_refinement_stochastic_ratio, 1.0,
    "*path_number over refinement_stochastic_ratio paths are extracted and "
    "*path_number paths are randomly selected from them for synthesis in each "
    "FDO iteration. Must be a positive float <= 1.0.");
ABSL_FLAG(std::string, fdo_path_evaluate_strategy, "window",
          "Support path, cone, and window for now");
ABSL_FLAG(std::string, fdo_synthesizer_name, "", "Only support yosys for now");
ABSL_FLAG(std::string, fdo_yosys_path, "", "Absolute path of Yosys");
ABSL_FLAG(std::string, fdo_sta_path, "", "Absolute path of OpenSTA");
ABSL_FLAG(std::string, fdo_synthesis_libraries, "",
          "Synthesis and STA libraries");
// LINT.ThenChange(
//   //xls/build_rules/xls_codegen_rules.bzl,
//   //docs_src/codegen_options.md
// )

namespace xls {

static absl::StatusOr<bool> SetOptionsFromFlags(
    SchedulingOptionsFlagsProto& proto) {
#define POPULATE_FLAG(__x)                                   \
  {                                                          \
    any_flags_set |= FLAGS_##__x.IsSpecifiedOnCommandLine(); \
    proto.set_##__x(absl::GetFlag(FLAGS_##__x));             \
  }
#define POPULATE_REPEATED_FLAG(__x)                                           \
  {                                                                           \
    any_flags_set |= FLAGS_##__x.IsSpecifiedOnCommandLine();                  \
    do {                                                                      \
      proto.mutable_##__x()->Clear();                                         \
      auto repeated_flag = absl::GetFlag(FLAGS_##__x);                        \
      proto.mutable_##__x()->Add(repeated_flag.begin(), repeated_flag.end()); \
    } while (0);                                                              \
  }
  bool any_flags_set = false;
  POPULATE_FLAG(clock_period_ps);
  POPULATE_FLAG(pipeline_stages);
  POPULATE_FLAG(delay_model);
  POPULATE_FLAG(clock_margin_percent);
  POPULATE_FLAG(period_relaxation_percent);
  POPULATE_FLAG(minimize_clock_on_failure);
  POPULATE_FLAG(worst_case_throughput);
  POPULATE_FLAG(additional_input_delay_ps);
  POPULATE_FLAG(ffi_fallback_delay_ps);
  POPULATE_REPEATED_FLAG(io_constraints);
  POPULATE_FLAG(receives_first_sends_last);
  POPULATE_FLAG(mutual_exclusion_z3_rlimit);
  POPULATE_FLAG(fdo_iteration_number);
  POPULATE_FLAG(fdo_delay_driven_path_number);
  POPULATE_FLAG(fdo_fanout_driven_path_number);
  POPULATE_FLAG(fdo_refinement_stochastic_ratio);
  POPULATE_FLAG(fdo_path_evaluate_strategy);
  POPULATE_FLAG(fdo_synthesizer_name);
  POPULATE_FLAG(fdo_yosys_path);
  POPULATE_FLAG(fdo_sta_path);
  POPULATE_FLAG(fdo_synthesis_libraries);
#undef POPULATE_FLAG
#undef POPULATE_REPEATED_FLAG
  return any_flags_set;
}

static absl::StatusOr<SchedulingOptionsFlagsProto>
GetSchedulingOptionsFlagsProto() {
  SchedulingOptionsFlagsProto proto;
  XLS_ASSIGN_OR_RETURN(bool any_individual_flags_set,
                       SetOptionsFromFlags(proto));
  if (any_individual_flags_set) {
    if (FLAGS_scheduling_options_proto.IsSpecifiedOnCommandLine()) {
      return absl::InvalidArgumentError(
          "Cannot combine 'scheduling_options_proto' and command line "
          "scheduling arguments");
    }
  } else if (FLAGS_scheduling_options_proto.IsSpecifiedOnCommandLine()) {
    XLS_RETURN_IF_ERROR(xls::ParseTextProtoFile(
        absl::GetFlag(FLAGS_scheduling_options_proto), &proto));
  }
  return proto;
}

static absl::StatusOr<SchedulingOptions> OptionsFromFlagProto(
    Package* p, const SchedulingOptionsFlagsProto& proto) {
  SchedulingOptions scheduling_options;
  if (proto.pipeline_stages() != 0) {
    scheduling_options.pipeline_stages(proto.pipeline_stages());
  }
  if (proto.clock_period_ps() != 0) {
    scheduling_options.clock_period_ps(proto.clock_period_ps());
  }
  if (proto.clock_margin_percent() != 0) {
    scheduling_options.clock_margin_percent(proto.clock_margin_percent());
  }
  if (proto.period_relaxation_percent() != 0) {
    scheduling_options.period_relaxation_percent(
        proto.period_relaxation_percent());
  }
  scheduling_options.minimize_clock_on_failure(
      proto.minimize_clock_on_failure());
  if (proto.worst_case_throughput() != 1) {
    scheduling_options.worst_case_throughput(proto.worst_case_throughput());
  }
  if (proto.additional_input_delay_ps() != 0) {
    scheduling_options.additional_input_delay_ps(
        proto.additional_input_delay_ps());
  }
  if (proto.ffi_fallback_delay_ps() != 0) {
    scheduling_options.ffi_fallback_delay_ps(proto.ffi_fallback_delay_ps());
  }

  for (const std::string& c : proto.io_constraints()) {
    std::vector<std::string> components = absl::StrSplit(c, ':');
    if (components.size() != 6) {
      return absl::InternalError(
          absl::StrFormat("Could not parse IO constraint: `%s`", c));
    }
    auto parse_dir =
        [&](const std::string& str) -> absl::StatusOr<IODirection> {
      if (str == "send") {
        return IODirection::kSend;
      }
      if (str == "recv") {
        return IODirection::kReceive;
      }
      return absl::InternalError(
          absl::StrFormat("Could not parse IO constraint: "
                          "invalid channel direction in `%s`",
                          c));
    };
    std::string source = components[0];
    XLS_ASSIGN_OR_RETURN(IODirection source_dir, parse_dir(components[1]));
    std::string target = components[2];
    XLS_ASSIGN_OR_RETURN(IODirection target_dir, parse_dir(components[3]));
    int64_t min_latency, max_latency;
    if (components[4] == "none") {
      min_latency = std::numeric_limits<int64_t>::min();
    } else if (!absl::SimpleAtoi(components[4], &min_latency)) {
      return absl::InternalError(
          absl::StrFormat("Could not parse IO constraint: "
                          "invalid minimum latency in `%s`",
                          c));
    }
    if (components[5] == "none") {
      max_latency = std::numeric_limits<int64_t>::max();
    } else if (!absl::SimpleAtoi(components[5], &max_latency)) {
      return absl::InternalError(
          absl::StrFormat("Could not parse IO constraint: "
                          "invalid maximum latency in `%s`",
                          c));
    }
    IOConstraint constraint(source, source_dir, target, target_dir, min_latency,
                            max_latency);
    scheduling_options.add_constraint(constraint);
  }
  if (proto.receives_first_sends_last()) {
    scheduling_options.add_constraint(RecvsFirstSendsLastConstraint());
  }
  if (proto.mutual_exclusion_z3_rlimit() != -1) {
    scheduling_options.mutual_exclusion_z3_rlimit(
        proto.mutual_exclusion_z3_rlimit());
  }

  if (p != nullptr) {
    for (const SchedulingConstraint& c : scheduling_options.constraints()) {
      if (std::holds_alternative<IOConstraint>(c)) {
        IOConstraint io_constr = std::get<IOConstraint>(c);
        if (!p->GetChannel(io_constr.SourceChannel()).ok()) {
          return absl::InternalError(absl::StrFormat(
              "Invalid channel name in IO constraint: %s; "
              "this name did not correspond to any channel in the package",
              io_constr.SourceChannel()));
        }
        if (!p->GetChannel(io_constr.TargetChannel()).ok()) {
          return absl::InternalError(absl::StrFormat(
              "Invalid channel name in IO constraint: %s; "
              "this name did not correspond to any channel in the package",
              io_constr.TargetChannel()));
        }
      }
    }
  }

  if (proto.fdo_iteration_number() < 1) {
    return absl::InternalError("fdo_iteration_number must be >= 1");
  }
  scheduling_options.fdo_iteration_number(proto.fdo_iteration_number());

  if (proto.fdo_delay_driven_path_number() < 0) {
    return absl::InternalError("delay_driven_path_number must be >= 0");
  }
  scheduling_options.fdo_delay_driven_path_number(
      proto.fdo_delay_driven_path_number());

  if (proto.fdo_fanout_driven_path_number() < 0) {
    return absl::InternalError("fanout_driven_path_number must be >= 0");
  }
  scheduling_options.fdo_fanout_driven_path_number(
      proto.fdo_fanout_driven_path_number());

  if (proto.fdo_refinement_stochastic_ratio() > 1.0 ||
      proto.fdo_refinement_stochastic_ratio() <= 0.0) {
    return absl::InternalError(
        "refinement_stochastic_ratio must be <= 1.0 and > 0.0");
  }
  scheduling_options.fdo_refinement_stochastic_ratio(
      proto.fdo_refinement_stochastic_ratio());

  if (proto.fdo_path_evaluate_strategy() != "path" &&
      proto.fdo_path_evaluate_strategy() != "cone" &&
      proto.fdo_path_evaluate_strategy() != "window") {
    return absl::InternalError(
        "path_evaluate_strategy must be 'path', 'cone', or 'window'");
  }
  scheduling_options.fdo_path_evaluate_strategy(
      proto.fdo_path_evaluate_strategy());

  scheduling_options.fdo_synthesizer_name(proto.fdo_synthesizer_name());

  return scheduling_options;
}

absl::StatusOr<DelayEstimator*> SetUpDelayEstimator() {
  XLS_ASSIGN_OR_RETURN(SchedulingOptionsFlagsProto proto,
                       GetSchedulingOptionsFlagsProto());
  return GetDelayEstimator(proto.delay_model());
}

absl::StatusOr<bool> IsDelayModelSpecifiedViaFlag() {
  XLS_ASSIGN_OR_RETURN(SchedulingOptionsFlagsProto proto,
                       GetSchedulingOptionsFlagsProto());
  return !proto.delay_model().empty();
}

absl::StatusOr<SchedulingOptions> SetUpSchedulingOptions(Package* p) {
  XLS_ASSIGN_OR_RETURN(SchedulingOptionsFlagsProto proto,
                       GetSchedulingOptionsFlagsProto());
  return OptionsFromFlagProto(p, proto);
}

absl::StatusOr<synthesis::Synthesizer*> SetUpSynthesizer() {
  XLS_ASSIGN_OR_RETURN(SchedulingOptionsFlagsProto proto,
                       GetSchedulingOptionsFlagsProto());
  if (proto.fdo_synthesizer_name() == "yosys") {
    if (proto.fdo_yosys_path().empty() || proto.fdo_sta_path().empty() ||
        proto.fdo_synthesis_libraries().empty()) {
      return absl::InternalError(
          "yosys_path, sta_path, and synthesis_libraries must not be empty");
    }
    synthesis::YosysSynthesizer* yosys_synthesizer =
        new synthesis::YosysSynthesizer(proto.fdo_yosys_path(),
                                        proto.fdo_sta_path(),
                                        proto.fdo_synthesis_libraries());
    return yosys_synthesizer;
  }

  return absl::InternalError("Synthesis service is invalid");
}

}  // namespace xls
