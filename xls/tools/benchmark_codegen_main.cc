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

#include <iostream>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_split.h"
#include "absl/time/time.h"
#include "xls/codegen/block_metrics.h"
#include "xls/codegen/xls_metrics.pb.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/status_macros.h"
#include "xls/delay_model/delay_estimator.h"
#include "xls/ir/block.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/package.h"
#include "xls/scheduling/pipeline_schedule.h"
#include "xls/scheduling/run_pipeline_schedule.h"
#include "xls/scheduling/scheduling_options.h"
#include "xls/tools/scheduling_options_flags.h"
#include "xls/tools/scheduling_options_flags.pb.h"

const char kUsage[] = R"(
Dumps various codegen-related metrics about a block and corresponding Verilog
file. Designed to be used with run_benchmarks.py script.

Usage:
   benchmark_codegen_main --delay_model=DELAY_MODEL \
     OPT_IR_FILE BLOCK_IR_FILE VERILOG_FILE
)";

ABSL_FLAG(std::string, top, "",
          "Name of top block to use in lieu of the default.");
ABSL_FLAG(bool, schedule, true, "Enable running the scheduler.");

namespace xls {
namespace {

absl::Status ScheduleAndPrintStats(Package* package,
                                   const DelayEstimator& delay_estimator,
                                   const SchedulingOptions& options) {
  std::optional<FunctionBase*> top = package->GetTop();
  if (!top.has_value()) {
    return absl::InternalError(absl::StrFormat(
        "Top entity not set for package: %s.", package->name()));
  }
  absl::Time start = absl::Now();
  XLS_ASSIGN_OR_RETURN(
      PipelineSchedule schedule,
      RunPipelineSchedule(top.value(), delay_estimator, options));
  absl::Duration total_time = absl::Now() - start;
  std::cout << absl::StreamFormat("Scheduling time: %dms\n",
                                  total_time / absl::Milliseconds(1));

  return absl::OkStatus();
}

absl::StatusOr<Block*> GetTopBlock(Package* package) {
  if (!absl::GetFlag(FLAGS_top).empty()) {
    return package->GetBlock(absl::GetFlag(FLAGS_top));
  }
  std::optional<FunctionBase*> top = package->GetTop();
  if (!top.has_value()) {
    return absl::InvalidArgumentError("Package has no top defined");
  }

  if (!top.value()->IsBlock()) {
    return absl::InvalidArgumentError(absl::StrFormat(
        "Top entity of package is not a block: %s", top.value()->name()));
  }
  return top.value()->AsBlockOrDie();
}

absl::Status RealMain(std::string_view opt_ir_path,
                      std::string_view block_ir_path,
                      std::string_view verilog_path) {
  XLS_VLOG(1) << "Reading optimized IR file: " << opt_ir_path;
  XLS_ASSIGN_OR_RETURN(std::string opt_ir_contents,
                       GetFileContents(opt_ir_path));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> opt_package,
                       Parser::ParsePackage(opt_ir_contents));

  XLS_VLOG(1) << "Reading block IR file: " << opt_ir_path;
  XLS_ASSIGN_OR_RETURN(std::string block_ir_contents,
                       GetFileContents(block_ir_path));
  XLS_ASSIGN_OR_RETURN(std::unique_ptr<Package> block_package,
                       Parser::ParsePackage(block_ir_contents));

  XLS_VLOG(1) << "Reading Verilog file: " << verilog_path;
  XLS_ASSIGN_OR_RETURN(std::string verilog_contents,
                       GetFileContents(verilog_path));

  const DelayEstimator* delay_estimator = nullptr;

  if (absl::GetFlag(FLAGS_schedule)) {
    XLS_ASSIGN_OR_RETURN(
        SchedulingOptionsFlagsProto scheduling_options_flags_proto,
        GetSchedulingOptionsFlagsProto());
    XLS_ASSIGN_OR_RETURN(SchedulingOptions scheduling_options,
                         SetUpSchedulingOptions(scheduling_options_flags_proto,
                                                block_package.get()));
    XLS_ASSIGN_OR_RETURN(delay_estimator,
                         SetUpDelayEstimator(scheduling_options_flags_proto));

    XLS_RETURN_IF_ERROR(ScheduleAndPrintStats(
        opt_package.get(), *delay_estimator, scheduling_options));
  }

  XLS_ASSIGN_OR_RETURN(Block * top, GetTopBlock(block_package.get()));
  XLS_ASSIGN_OR_RETURN(verilog::BlockMetricsProto metrics,
                       verilog::GenerateBlockMetrics(top, delay_estimator));
  std::cout << absl::StreamFormat("Flop count: %d\n", metrics.flop_count());
  std::cout << absl::StreamFormat(
      "Has feedthrough path: %s\n",
      metrics.feedthrough_path_exists() ? "true" : "false");
  if (metrics.has_max_reg_to_reg_delay_ps()) {
    std::cout << absl::StreamFormat("Max reg-to-reg delay: %dps\n",
                                    metrics.max_reg_to_reg_delay_ps());
  }
  if (metrics.has_max_input_to_reg_delay_ps()) {
    std::cout << absl::StreamFormat("Max input-to-reg delay: %dps\n",
                                    metrics.max_input_to_reg_delay_ps());
  }
  if (metrics.has_max_reg_to_output_delay_ps()) {
    std::cout << absl::StreamFormat("Max reg-to-output delay: %dps\n",
                                    metrics.max_reg_to_output_delay_ps());
  }
  if (metrics.has_max_feedthrough_path_delay_ps()) {
    std::cout << absl::StreamFormat("Max feedthrough path delay: %dps\n",
                                    metrics.max_feedthrough_path_delay_ps());
  }
  std::cout << absl::StreamFormat(
      "Lines of Verilog: %d\n",
      std::vector<std::string>(absl::StrSplit(verilog_contents, '\n')).size());

  return absl::OkStatus();
}

}  // namespace
}  // namespace xls

int main(int argc, char** argv) {
  std::vector<std::string_view> positional_arguments =
      xls::InitXls(kUsage, argc, argv);

  if (positional_arguments.size() != 3) {
    XLS_LOG(QFATAL) << absl::StreamFormat(
        "Expected invocation:\n  %s OPT_IR_FILE BLOCK_IR_FILE VERILOG_FILE",
        argv[0]);
  }

  return xls::ExitStatus(xls::RealMain(positional_arguments[0],
                                       positional_arguments[1],
                                       positional_arguments[2]));
}
