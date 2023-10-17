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

// Tool to evaluate the behavior of a Proc network.

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <iostream>
#include <memory>
#include <numeric>
#include <optional>
#include <queue>
#include <random>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/flags/flag.h"
#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "absl/strings/escaping.h"
#include "absl/strings/match.h"
#include "absl/strings/numbers.h"
#include "absl/strings/str_format.h"
#include "absl/strings/str_join.h"
#include "absl/strings/str_split.h"
#include "absl/types/span.h"
#include "xls/codegen/module_signature.pb.h"
#include "xls/common/exit_status.h"
#include "xls/common/file/filesystem.h"
#include "xls/common/init_xls.h"
#include "xls/common/logging/logging.h"
#include "xls/common/status/ret_check.h"
#include "xls/common/status/status_macros.h"
#include "xls/interpreter/block_interpreter.h"
#include "xls/interpreter/channel_queue.h"
#include "xls/interpreter/interpreter_proc_runtime.h"
#include "xls/interpreter/serial_proc_runtime.h"
#include "xls/ir/bits.h"
#include "xls/ir/events.h"
#include "xls/ir/function_builder.h"
#include "xls/ir/ir_parser.h"
#include "xls/ir/nodes.h"
#include "xls/ir/package.h"
#include "xls/ir/register.h"
#include "xls/ir/value.h"
#include "xls/ir/value_helpers.h"
#include "xls/jit/jit_proc_runtime.h"
#include "xls/tools/eval_helpers.h"

constexpr const char* kUsage = R"(
Evaluates an IR file containing Procs, or a Block generated from them.
The Proc network will be ticked a fixed number of times
(specified on the command line) and the final state
value of each proc will be printed to the terminal upon completion.

Initial states are set according to their declarations inside the IR itself.
)";

ABSL_FLAG(std::vector<std::string>, ticks, {},
          "Can be a comma-separated list of runs. "
          "Number of clock ticks to execute for each, with proc state "
          "resetting per run. <0 runs until all outputs are verified.");
ABSL_FLAG(std::string, backend, "serial_jit",
          "Backend to use for evaluation. Valid options are:\n"
          " * serial_jit: JIT-backed single-stepping runtime.\n"
          " * ir_interpreter: Interpreter at the IR level.\n"
          " * block_interpreter: Interpret a block generated from a proc.");
ABSL_FLAG(std::string, block_signature_proto, "",
          "Path to textproto file containing signature from codegen");
ABSL_FLAG(int64_t, max_cycles_no_output, 100,
          "For block simulation, stop after this many cycles without output.");
ABSL_FLAG(
    std::vector<std::string>, inputs_for_channels, {},
    "Comma separated list of channel=filename pairs, for example: ch_a=foo.ir. "
    "Files contain one XLS Value in human-readable form per line. Either "
    "'inputs_for_channels' or 'inputs_for_all_channels' can be defined.");
ABSL_FLAG(
    std::vector<std::string>, expected_outputs_for_channels, {},
    "Comma separated list of channel=filename pairs, for example: ch_a=foo.ir. "
    "Files contain one XLS Value in human-readable form per line. Either "
    "'expected_outputs_for_channels' or 'expected_outputs_for_all_channels' "
    "can be defined.\n"
    "For procs, when 'expected_outputs_for_channels' or "
    "'expected_outputs_for_all_channels' are not specified the values of all "
    "the channel are displayed on stdout.");
ABSL_FLAG(
    std::string, inputs_for_all_channels, "",
    "Path to file containing inputs for all channels.\n"
    "The file format is:\n"
    "CHANNEL_NAME : {\n"
    "  VALUE\n"
    "}\n"
    "where CHANNEL_NAME is the name of the channel and VALUE is one XLS Value "
    "in human-readable form. There is one VALUE per line. There may be zero or "
    "more occurences of VALUE for a channel. The file may contain one or more "
    "channels. Either 'inputs_for_channels' or 'inputs_for_all_channels' can "
    "be defined.");
ABSL_FLAG(
    std::string, expected_outputs_for_all_channels, "",
    "Path to file containing outputs for all channels.\n"
    "The file format is:\n"
    "CHANNEL_NAME : {\n"
    "  VALUE\n"
    "}\n"
    "where CHANNEL_NAME is the name of the channel and VALUE is one XLS Value "
    "in human-readable form. There is one VALUE per line. There may be zero or "
    "more occurences of VALUE for a channel. The file may contain one or more "
    "channels. Either 'expected_outputs_for_channels' or "
    "'expected_outputs_for_all_channels' can be defined.\n"
    "For procs, when 'expected_outputs_for_channels' or "
    "'expected_outputs_for_all_channels' are not specified the values of all "
    "the channel are displayed on stdout.");
ABSL_FLAG(std::string, streaming_channel_data_suffix, "_data",
          "Suffix to data signals for streaming channels.");
ABSL_FLAG(std::string, streaming_channel_valid_suffix, "_vld",
          "Suffix to valid signals for streaming channels.");
ABSL_FLAG(std::string, streaming_channel_ready_suffix, "_rdy",
          "Suffix to ready signals for streaming channels.");
ABSL_FLAG(std::string, memory_read_enable_suffix, "_rd_en",
          "Suffix to memory port for read enable.");
ABSL_FLAG(std::string, memory_read_address_suffix, "_rd_addr",
          "Suffix to memory port for read address.");
ABSL_FLAG(std::string, memory_read_data_suffix, "_rd_data",
          "Suffix to memory port for read data.");
ABSL_FLAG(std::string, memory_write_enable_suffix, "_wr_en",
          "Suffix to memory port for write enable.");
ABSL_FLAG(std::string, memory_write_address_suffix, "_wr_addr",
          "Suffix to memory port for write address.");
ABSL_FLAG(std::string, memory_write_data_suffix, "_wr_data",
          "Suffix to memory port for write data.");
ABSL_FLAG(std::string, idle_channel_name, "idle", "Name of idle channel.");
ABSL_FLAG(int64_t, random_seed, 42, "Random seed");
ABSL_FLAG(double, prob_input_valid_assert, 1.0,
          "Single-cycle probability of asserting valid with more input ready.");
ABSL_FLAG(bool, show_trace, false, "Whether or not to print trace messages.");
ABSL_FLAG(std::string, output_stats_path, "", "File to output statistics to.");
ABSL_FLAG(std::vector<std::string>, model_memories, {},
          "Comma separated list of memory=depth/element_type:initial_value "
          "pairs, for example: "
          "mem=32/bits[32]:0");

namespace xls {

static absl::Status LogInterpreterEvents(std::string_view entity_name,
                                         const InterpreterEvents& events) {
  if (absl::GetFlag(FLAGS_show_trace)) {
    for (const auto& msg : events.trace_msgs) {
      std::string unescaped_msg;
      XLS_RET_CHECK(absl::CUnescape(msg, &unescaped_msg));
      std::cerr << "Proc " << entity_name << " trace: " << unescaped_msg
                << "\n";
    }
    for (const auto& msg : events.assert_msgs) {
      std::string unescaped_msg;
      XLS_RET_CHECK(absl::CUnescape(msg, &unescaped_msg));
      std::cerr << "Proc " << entity_name << " assert: " << unescaped_msg
                << "\n";
    }
  }
  return absl::OkStatus();
}

static absl::Status EvaluateProcs(
    Package* package, bool use_jit, const std::vector<int64_t>& ticks,
    const absl::flat_hash_map<std::string, std::vector<Value>>&
        inputs_for_channels,
    absl::flat_hash_map<std::string, std::vector<Value>>&
        expected_outputs_for_channels) {
  std::unique_ptr<SerialProcRuntime> runtime;
  if (use_jit) {
    XLS_ASSIGN_OR_RETURN(runtime, CreateJitSerialProcRuntime(package));
  } else {
    XLS_ASSIGN_OR_RETURN(runtime, CreateInterpreterSerialProcRuntime(package));
  }

  ChannelQueueManager& queue_manager = runtime->queue_manager();
  for (const auto& [channel_name, values] : inputs_for_channels) {
    XLS_ASSIGN_OR_RETURN(ChannelQueue * in_queue,
                         queue_manager.GetQueueByName(channel_name));
    for (const Value& value : values) {
      XLS_RETURN_IF_ERROR(in_queue->Write(value));
    }
  }

  for (int64_t this_ticks : ticks) {
    if (absl::GetFlag(FLAGS_show_trace)) {
      XLS_LOG(INFO) << "Resetting proc state";
    }
    runtime->ResetState();

    for (int i = 0; this_ticks < 0 || i < this_ticks; i++) {
      if (absl::GetFlag(FLAGS_show_trace)) {
        XLS_LOG(INFO) << "Tick " << i;
      }
      // Don't double print events (traces, assertions, etc)
      runtime->ClearInterpreterEvents();
      XLS_RETURN_IF_ERROR(runtime->Tick());

      // Sort the keys for stable print order.
      absl::flat_hash_map<Proc*, std::vector<Value>> states;
      std::vector<Proc*> sorted_procs;
      for (const auto& proc : package->procs()) {
        sorted_procs.push_back(proc.get());
        states[proc.get()] = runtime->ResolveState(proc.get());
      }

      std::sort(sorted_procs.begin(), sorted_procs.end(),
                [](Proc* a, Proc* b) { return a->name() < b->name(); });

      for (Proc* proc : sorted_procs) {
        XLS_RETURN_IF_ERROR(LogInterpreterEvents(
            proc->name(), runtime->GetInterpreterEvents(proc)));
      }

      for (const auto& proc : package->procs()) {
        const auto& state = states.at(proc.get());
        XLS_VLOG(1) << "Proc " << proc->name() << " : "
                    << absl::StrFormat(
                           "{%s}", absl::StrJoin(state, ", ", ValueFormatter));
      }

      // --ticks 0 stops when all outputs are verified
      if (this_ticks < 0) {
        bool all_outputs_verified = true;
        for (const auto& [channel_name, values] :
             expected_outputs_for_channels) {
          XLS_ASSIGN_OR_RETURN(ChannelQueue * out_queue,
                               queue_manager.GetQueueByName(channel_name));
          if (out_queue->GetSize() < values.size()) {
            all_outputs_verified = false;
          }
        }
        if (all_outputs_verified) {
          break;
        }
      }
    }
  }

  bool checked_any_output = false;
  for (const auto& [channel_name, values] : expected_outputs_for_channels) {
    XLS_ASSIGN_OR_RETURN(ChannelQueue * out_queue,
                         queue_manager.GetQueueByName(channel_name));
    uint64_t processed_count = 0;
    for (const Value& value : values) {
      std::optional<Value> out_val = out_queue->Read();
      if (!out_val.has_value()) {
        return absl::UnknownError(absl::StrFormat(
            "Channel %s didn't consume %d expected values (processed %d)",
            channel_name, values.size() - processed_count, processed_count));
      }
      if (value != *out_val) {
        XLS_RET_CHECK_EQ(value, *out_val) << absl::StreamFormat(
            "Mismatched (channel=%s) after %d outputs (%s != %s)", channel_name,
            processed_count, value.ToString(), out_val->ToString());
      }
      checked_any_output = true;
      ++processed_count;
    }
  }

  if (!checked_any_output && !expected_outputs_for_channels.empty()) {
    return absl::UnknownError("No output verified (empty expected values?)");
  }

  if (expected_outputs_for_channels.empty()) {
    for (const Channel* channel : package->channels()) {
      if (!channel->CanSend()) {
        continue;
      }
      XLS_ASSIGN_OR_RETURN(ChannelQueue * out_queue,
                           queue_manager.GetQueueByName(channel->name()));
      std::vector<Value> channel_values(out_queue->GetSize());
      int64_t index = 0;
      while (!out_queue->IsEmpty()) {
        std::optional<Value> out_val = out_queue->Read();
        channel_values[index++] = out_val.value();
      }
      expected_outputs_for_channels.insert({channel->name(), channel_values});
    }
    std::cout << ChannelValuesToString(expected_outputs_for_channels);
  }

  return absl::OkStatus();
}

struct ChannelInfo {
  int64_t width = -1;
  bool port_input = false;
  // Exactly 2 for ready/valid
  int ready_valid = 0;

  // Precalculated channel names
  std::string channel_ready;
  std::string channel_valid;
  std::string channel_data;
};

static absl::StatusOr<absl::flat_hash_map<std::string, ChannelInfo>>
InterpretBlockSignature(
    Block* block, const verilog::ModuleSignatureProto& signature,
    const absl::flat_hash_map<std::string, std::vector<Value>>&
        inputs_for_channels,
    const absl::flat_hash_map<std::string, std::vector<Value>>&
        expected_outputs_for_channels,
    const absl::flat_hash_map<std::string, std::pair<int64_t, Value>>&
        model_memories,
    std::string_view streaming_channel_data_suffix,
    std::string_view streaming_channel_ready_suffix,
    std::string_view streaming_channel_valid_suffix,
    std::string_view memory_read_enable_suffix,
    std::string_view memory_read_address_suffix,
    std::string_view memory_read_data_suffix,
    std::string_view memory_write_enable_suffix,
    std::string_view memory_write_address_suffix,
    std::string_view memory_write_data_suffix,
    std::string_view idle_channel_name) {
  absl::flat_hash_set<std::string> memory_port_names;
  for (const auto& [name, _] : model_memories) {
    memory_port_names.insert(name + std::string(memory_read_enable_suffix));
    memory_port_names.insert(name + std::string(memory_read_address_suffix));
    memory_port_names.insert(name + std::string(memory_read_data_suffix));
    memory_port_names.insert(name + std::string(memory_write_enable_suffix));
    memory_port_names.insert(name + std::string(memory_write_address_suffix));
    memory_port_names.insert(name + std::string(memory_write_data_suffix));
  }

  absl::flat_hash_map<std::string, ChannelInfo> channel_info;

  for (const xls::verilog::PortProto& port : signature.data_ports()) {
    if (memory_port_names.contains(port.name())) {
      continue;
    }

    if (absl::EndsWith(port.name(), streaming_channel_data_suffix)) {
      std::string port_name = port.name().substr(0, port.name().size() - 5);
      XLS_CHECK(!channel_info.contains(port_name));
      channel_info[port_name].width = port.width();
      if (port.direction() == verilog::DIRECTION_INPUT) {
        channel_info[port_name].port_input = true;
      } else if (port.direction() == verilog::DIRECTION_OUTPUT) {
        channel_info[port_name].port_input = false;
      } else {
        return absl::InvalidArgumentError(absl::StrFormat(
            "Don't understand port direction: %i", port.direction()));
      }
    }

    bool this_port_input;

    if (port.direction() == verilog::DIRECTION_INPUT) {
      this_port_input = true;
    } else if (port.direction() == verilog::DIRECTION_OUTPUT) {
      this_port_input = false;
    } else {
      return absl::InvalidArgumentError(absl::StrFormat(
          "Don't understand port direction: %i", port.direction()));
    }

    bool ready_valid = false;

    std::string port_name;
    if (absl::EndsWith(port.name(), streaming_channel_data_suffix)) {
      port_name = port.name().substr(
          0, port.name().size() - streaming_channel_data_suffix.size());
    } else if (absl::EndsWith(port.name(), streaming_channel_ready_suffix)) {
      port_name = port.name().substr(
          0, port.name().size() - streaming_channel_ready_suffix.size());
      ready_valid = true;
      XLS_CHECK(channel_info.contains(port_name));
      XLS_CHECK(this_port_input != channel_info.at(port_name).port_input);
    } else if (absl::EndsWith(port.name(), streaming_channel_valid_suffix)) {
      port_name = port.name().substr(
          0, port.name().size() - streaming_channel_valid_suffix.size());
      ready_valid = true;
      XLS_CHECK(channel_info.contains(port_name));
      XLS_CHECK(this_port_input == channel_info.at(port_name).port_input);
    } else if (port.name() == idle_channel_name) {
      continue;
    } else {
      port_name = port.name();
      XLS_LOG(WARNING) << "Warning: Assuming port " << port_name
                       << " is single value, or direct, input";
      XLS_CHECK(this_port_input);
      ready_valid = false;
      channel_info[port_name].port_input = true;
      channel_info[port_name].width = port.width();
    }

    XLS_CHECK(channel_info.contains(port_name));
    if (ready_valid) {
      ++channel_info[port_name].ready_valid;
    }
  }

  for (auto& [name, info] : channel_info) {
    XLS_CHECK(info.ready_valid == 0 || info.ready_valid == 2);

    if (info.port_input) {
      XLS_CHECK(inputs_for_channels.contains(name));
    } else {
      XLS_CHECK(expected_outputs_for_channels.contains(name));
    }

    info.channel_ready = name + std::string(streaming_channel_ready_suffix);
    info.channel_valid = name + std::string(streaming_channel_valid_suffix);
    info.channel_data = name + std::string(streaming_channel_data_suffix);
  }

  for (const auto& [name, _] : inputs_for_channels) {
    if (!channel_info.contains(name)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Channel %s should not be in channel inputs file, as "
                          "there are no corresponding ports",
                          name));
    }
  }
  for (const auto& [name, _] : expected_outputs_for_channels) {
    if (!channel_info.contains(name)) {
      return absl::InvalidArgumentError(
          absl::StrFormat("Channel %s should not be in channel outputs file, "
                          "as there are no corresponding ports",
                          name));
    }
  }

  return channel_info;
}

class MemoryModel {
 public:
  MemoryModel(const std::string& name, size_t size, const Value& initial_value,
              const Value& read_disabled_value, bool show_trace)
      : name_(name),
        read_disabled_value_(read_disabled_value),
        show_trace_(show_trace) {
    cells_.resize(size, initial_value);
  }
  absl::Status Read(int64_t addr) {
    if (addr < 0 || addr >= cells_.size()) {
      return absl::OutOfRangeError(
          absl::StrFormat("Memory %s read out of range at %i", name_, addr));
    }
    if (read_this_tick_.has_value()) {
      return absl::FailedPreconditionError(
          absl::StrFormat("Memory %s double read in tick at %i", name_, addr));
    }
    read_this_tick_ = cells_[addr];
    if (show_trace_) {
      XLS_LOG(INFO) << "Memory Model: Initiated read " << name_ << "[" << addr
                    << "] = " << read_this_tick_.value();
    }
    return absl::OkStatus();
  }
  Value GetValueReadLastTick() const {
    if (show_trace_) {
      if (read_last_tick_.has_value()) {
        XLS_LOG(INFO) << "Memory Model: Got read last value " << name_ << " = "
                      << read_last_tick_.value();
      } else {
        XLS_LOG(INFO) << "Memory Model: Got read last default " << name_
                      << " = " << read_disabled_value_;
      }
    }
    return read_last_tick_.has_value() ? read_last_tick_.value()
                                       : read_disabled_value_;
  }
  bool DidReadLastTick() const { return read_last_tick_.has_value(); }
  absl::Status Write(int64_t addr, const Value& value) {
    if (addr < 0 || addr >= cells_.size()) {
      return absl::OutOfRangeError(
          absl::StrFormat("Memory %s write out of range at %i", name_, addr));
    }
    if (write_this_tick_.has_value()) {
      return absl::FailedPreconditionError(
          absl::StrFormat("Memory %s double write in tick at %i", name_, addr));
    }
    if (value.GetFlatBitCount() != cells_[0].GetFlatBitCount()) {
      return absl::FailedPreconditionError(absl::StrFormat(
          "Memory %s write value at %i with wrong bit count %i, expected %i",
          name_, addr, value.GetFlatBitCount(), cells_[0].GetFlatBitCount()));
    }
    if (show_trace_) {
      XLS_LOG(INFO) << "Memory Model: Initiated write " << name_ << "[" << addr
                    << "] = " << value;
    }
    write_this_tick_ = std::make_pair(addr, value);
    return absl::OkStatus();
  }
  absl::Status Tick() {
    if (write_this_tick_.has_value()) {
      if (show_trace_) {
        XLS_LOG(INFO) << "Memory Model: Committed write " << name_ << "["
                      << write_this_tick_->first
                      << "] = " << write_this_tick_->second;
      }
      cells_[write_this_tick_->first] = write_this_tick_->second;
      write_this_tick_.reset();
    }
    read_last_tick_ = read_this_tick_;
    read_this_tick_.reset();
    return absl::OkStatus();
  }

 private:
  const std::string name_;
  const Value read_disabled_value_;
  std::vector<Value> cells_;
  std::optional<std::pair<int64_t, Value>> write_this_tick_;
  std::optional<Value> read_this_tick_;
  std::optional<Value> read_last_tick_;
  const bool show_trace_;
};

// XLS doesn't have X. Fill with all 1s, as this is generally more likely
// to expose logical problems.
static Value XsOfType(Type* type) { return AllOnesOfType(type); }

static xls::Type* GetPortTypeOrNull(Block* block, std::string_view port_name) {
  for (const InputPort* port : block->GetInputPorts()) {
    if (port->name() == port_name) {
      return port->GetType();
    }
  }
  return nullptr;
}

static absl::Status RunBlockInterpreter(
    Package* package, const std::vector<int64_t>& ticks,
    const verilog::ModuleSignatureProto& signature,
    const int64_t max_cycles_no_output,
    absl::flat_hash_map<std::string, std::vector<Value>> inputs_for_channels,
    absl::flat_hash_map<std::string, std::vector<Value>>
        expected_outputs_for_channels,
    const absl::flat_hash_map<std::string, std::pair<int64_t, Value>>&
        model_memories_param,
    std::string_view streaming_channel_data_suffix,
    std::string_view streaming_channel_ready_suffix,
    std::string_view streaming_channel_valid_suffix,
    std::string_view memory_read_enable_suffix,
    std::string_view memory_read_address_suffix,
    std::string_view memory_read_data_suffix,
    std::string_view memory_write_enable_suffix,
    std::string_view memory_write_address_suffix,
    std::string_view memory_write_data_suffix,
    std::string_view idle_channel_name, const int random_seed,
    const double prob_input_valid_assert, bool show_trace,
    std::string_view output_stats_path) {
  if (package->blocks().size() != 1) {
    return absl::InvalidArgumentError(
        "Input IR should contain exactly one block");
  }

  std::default_random_engine rand_eng(random_seed);
  std::uniform_real_distribution<double> rand_distr(0.0, 1.0);

  Block* block = package->blocks()[0].get();

  // TODO: Support multiple resets
  XLS_CHECK_EQ(ticks.size(), 1);

  absl::flat_hash_map<std::string, ChannelInfo> channel_info;
  XLS_ASSIGN_OR_RETURN(
      channel_info,
      InterpretBlockSignature(
          block, signature, inputs_for_channels, expected_outputs_for_channels,
          model_memories_param, streaming_channel_data_suffix,
          streaming_channel_ready_suffix, streaming_channel_valid_suffix,
          memory_read_enable_suffix, memory_read_address_suffix,
          memory_read_data_suffix, memory_write_enable_suffix,
          memory_write_address_suffix, memory_write_data_suffix,
          idle_channel_name));

  // Prepare values in queue format
  absl::flat_hash_map<std::string, std::queue<Value>> channel_value_queues;
  for (const auto& [name, values] : inputs_for_channels) {
    XLS_CHECK(!channel_value_queues.contains(name));
    channel_value_queues[name] = std::queue<Value>();
    for (const Value& value : values) {
      channel_value_queues[name].push(value);
    }
  }
  for (const auto& [name, values] : expected_outputs_for_channels) {
    XLS_CHECK(!channel_value_queues.contains(name));
    channel_value_queues[name] = std::queue<Value>();
    for (const Value& value : values) {
      channel_value_queues[name].push(value);
    }
  }

  absl::flat_hash_map<std::string, std::unique_ptr<MemoryModel>> model_memories;

  for (const auto& [name, model_pair] : model_memories_param) {
    const std::string rd_data = name + std::string(memory_read_data_suffix);
    XLS_ASSIGN_OR_RETURN(const InputPort* port, block->GetInputPort(rd_data));
    model_memories[name] = std::make_unique<MemoryModel>(
        name, model_pair.first, model_pair.second,
        /*read_disabled_value=*/XsOfType(port->GetType()), show_trace);
  }

  // Initial register state is one for all registers.
  // Ideally this would be randomized, but at least 1s are more likely to
  //  expose bad behavior than 0s.
  absl::flat_hash_map<std::string, Value> reg_state;
  for (Register* reg : block->GetRegisters()) {
    Value def = ZeroOfType(reg->type());
    reg_state[reg->name()] = XsOfType(reg->type());
  }

  int64_t last_output_cycle = 0;
  int64_t matched_outputs = 0;

  for (int64_t cycle = 0;; ++cycle) {
    // Idealized reset behavior
    const bool resetting = (cycle == 0);

    if (show_trace && ((cycle < 30) || (cycle % 100 == 0))) {
      XLS_LOG(INFO) << "Cycle[" << cycle << "]: resetting? " << resetting
                    << " matched outputs " << matched_outputs;
    }

    absl::flat_hash_set<std::string> asserted_valids;
    absl::flat_hash_map<std::string, Value> input_set;

    if (!signature.reset().name().empty()) {
      input_set[signature.reset().name()] = Value(
          xls::UBits((resetting ^ signature.reset().active_low()) ? 1 : 0, 1));
    } else {
      XLS_LOG(WARNING) << "No reset found in signature!";
    }

    for (const auto& [name, _] : inputs_for_channels) {
      const ChannelInfo& info = channel_info.at(name);
      const std::queue<Value>& queue = channel_value_queues.at(name);
      if (info.ready_valid != 0) {
        // Don't bring valid low without a transaction
        const bool asserted_valid = asserted_valids.contains(name);
        const bool random_go_head =
            rand_distr(rand_eng) <= prob_input_valid_assert;
        const bool this_valid =
            asserted_valid || (random_go_head && !queue.empty());
        if (this_valid) {
          asserted_valids.insert(name);
        }
        input_set[info.channel_valid] =
            Value(xls::UBits(this_valid ? 1 : 0, 1));
        // Channels without data port will return nullptr
        xls::Type* port_type = GetPortTypeOrNull(
            block, name + streaming_channel_data_suffix.data());

        if (port_type != nullptr) {
          input_set[info.channel_data] =
              queue.empty() ? XsOfType(port_type) : queue.front();
        }
      } else {
        // Just take the first value for the single value channels
        XLS_CHECK(!queue.empty());
        input_set[name] = queue.front();
      }
    }
    for (const auto& [name, model] : model_memories) {
      const std::string rd_data = name + std::string(memory_read_data_suffix);
      input_set[rd_data] = model->GetValueReadLastTick();
    }
    for (const auto& [name, _] : expected_outputs_for_channels) {
      const ChannelInfo& info = channel_info.at(name);
      XLS_CHECK(info.ready_valid);
      input_set[info.channel_ready] = Value(xls::UBits(1, 1));
    }
    XLS_ASSIGN_OR_RETURN(xls::BlockRunResult result,
                         xls::BlockRun(input_set, reg_state, block));
    reg_state = std::move(result.reg_state);

    // Output trace messages
    XLS_RETURN_IF_ERROR(
        LogInterpreterEvents(block->name(), result.interpreter_events));

    if (resetting) {
      last_output_cycle = cycle;
      continue;
    }

    // Channel output checks
    for (const auto& [name, _] : inputs_for_channels) {
      const ChannelInfo& info = channel_info.at(name);

      if (info.ready_valid == 0) {
        continue;
      }

      const bool vld_value = input_set.at(info.channel_valid).bits().Get(0);
      const bool rdy_value =
          result.outputs.at(info.channel_ready).bits().Get(0);

      std::queue<Value>& queue = channel_value_queues.at(name);

      if (vld_value && rdy_value) {
        if (show_trace) {
          XLS_LOG(INFO) << "Channel Model: Consuming input for " << name << ": "
                        << queue.front().ToString();
        }
        queue.pop();
        asserted_valids.erase(name);
      }
    }

    for (const auto& [name, _] : expected_outputs_for_channels) {
      const ChannelInfo& info = channel_info.at(name);

      const bool vld_value =
          result.outputs.at(info.channel_valid).bits().Get(0);
      const bool rdy_value = input_set.at(info.channel_ready).bits().Get(0);

      std::queue<Value>& queue = channel_value_queues.at(name);

      if (rdy_value && vld_value) {
        if (queue.empty()) {
          return absl::OutOfRangeError(
              absl::StrFormat("Block wrote past the end of the expected values "
                              "list for channel %s",
                              name));
        }
        const Value& data_value = result.outputs.at(info.channel_data);
        const Value& match_value = queue.front();
        if (show_trace) {
          XLS_LOG(INFO) << "Channel Model: Consuming output for " << name
                        << ": " << data_value << ", remaining " << queue.size();
        }
        if (match_value != data_value) {
          return absl::UnknownError(absl::StrFormat(
              "Output mismatched for channel %s: expected %s, block outputted "
              "%s",
              name, match_value.ToString(), data_value.ToString()));
        }
        ++matched_outputs;
        queue.pop();
        last_output_cycle = cycle;
      }
    }

    // Memory model outputs
    for (const auto& [name, model] : model_memories) {
      // Write handling
      {
        const std::string wr_addr =
            name + std::string(memory_write_address_suffix);
        const std::string wr_data =
            name + std::string(memory_write_data_suffix);
        const std::string wr_en =
            name + std::string(memory_write_enable_suffix);
        const Value wr_en_val = result.outputs.at(wr_en);
        XLS_CHECK(wr_en_val.IsBits());
        if (wr_en_val.IsAllOnes()) {
          const Value wr_addr_val = result.outputs.at(wr_addr);
          const Value wr_data_val = result.outputs.at(wr_data);
          XLS_CHECK(wr_addr_val.IsBits());
          XLS_CHECK(wr_data_val.IsBits());
          XLS_ASSIGN_OR_RETURN(uint64_t addr, wr_addr_val.bits().ToUint64());
          XLS_RETURN_IF_ERROR(model->Write(addr, wr_data_val));
        }
      }
      // Read handling
      {
        const std::string rd_addr =
            name + std::string(memory_read_address_suffix);
        const std::string rd_en = name + std::string(memory_read_enable_suffix);
        const Value rd_en_val = result.outputs.at(rd_en);
        XLS_CHECK(rd_en_val.IsBits());
        if (rd_en_val.IsAllOnes()) {
          const Value rd_addr_val = result.outputs.at(rd_addr);
          XLS_CHECK(rd_addr_val.IsBits());
          XLS_ASSIGN_OR_RETURN(uint64_t addr, rd_addr_val.bits().ToUint64());
          XLS_RETURN_IF_ERROR(model->Read(addr));
        }
      }
    }

    bool all_queues_empty = true;

    for (const auto& [name, queue] : channel_value_queues) {
      // Ignore single value channels in this check
      const ChannelInfo& info = channel_info.at(name);
      if (info.ready_valid == 0) {
        continue;
      }

      if (!queue.empty()) {
        all_queues_empty = false;
      }
    }

    if (all_queues_empty) {
      break;
    }

    // Break on no output for too long
    if ((cycle - last_output_cycle) > max_cycles_no_output) {
      return absl::OutOfRangeError(absl::StrFormat(
          "Block didn't produce output for %i cycles", max_cycles_no_output));
    }

    for (const auto& [_, model] : model_memories) {
      XLS_RETURN_IF_ERROR(model->Tick());
    }
  }

  if (!output_stats_path.empty()) {
    XLS_RETURN_IF_ERROR(xls::SetFileContents(
        output_stats_path, absl::StrFormat("%i", last_output_cycle)));
  }

  return absl::OkStatus();
}

static absl::StatusOr<absl::flat_hash_map<std::string, std::string>>
ParseChannelFilenames(absl::Span<const std::string> files_raw) {
  absl::flat_hash_map<std::string, std::string> ret;
  for (const std::string& file : files_raw) {
    std::vector<std::string> split = absl::StrSplit(file, '=');
    if (split.size() != 2) {
      return absl::InvalidArgumentError(
          "Format of argument should be channel=file");
    }

    ret[split[0]] = split[1];
  }
  return ret;
}

absl::StatusOr<absl::flat_hash_map<
    std::string,
    std::pair<int64_t,
              Value>>> static ParseMemoryModels(absl::Span<const std::string>
                                                    models_raw) {
  absl::flat_hash_map<std::string, std::pair<int64_t, Value>> ret;
  for (const std::string& model_str : models_raw) {
    std::vector<std::string> split = absl::StrSplit(model_str, '=');
    if (split.size() != 2) {
      return absl::InvalidArgumentError(
          "Format of argument should be memory=size/initial_value");
    }
    std::vector<std::string> model_split = absl::StrSplit(split[1], '/');
    if (model_split.size() != 2) {
      return absl::InvalidArgumentError(
          "Format of memory model should be size/initial_value");
    }
    int64_t size = -1;
    if (!absl::SimpleAtoi(model_split[0], &size)) {
      return absl::InvalidArgumentError("Size should be an integer");
    }
    XLS_ASSIGN_OR_RETURN(Value initial_value,
                         Parser::ParseTypedValue(model_split[1]));
    ret[split[0]] = std::make_pair(size, initial_value);
  }
  return ret;
}

static absl::StatusOr<absl::flat_hash_map<std::string, std::vector<Value>>>
GetValuesForEachChannels(
    absl::Span<const std::string> filenames_for_each_channel,
    const int64_t total_ticks) {
  absl::flat_hash_map<std::string, std::string> channel_filenames;
  XLS_ASSIGN_OR_RETURN(channel_filenames,
                       ParseChannelFilenames(filenames_for_each_channel));
  absl::flat_hash_map<std::string, std::vector<Value>> values_for_channels;

  for (const auto& [channel_name, filename] : channel_filenames) {
    XLS_ASSIGN_OR_RETURN(std::vector<Value> values,
                         ParseValuesFile(filename, total_ticks));
    values_for_channels[channel_name] = values;
  }
  return values_for_channels;
}

static absl::Status RealMain(
    std::string_view ir_file, std::string_view backend,
    std::string_view block_signature_proto, std::vector<int64_t> ticks,
    const int64_t max_cycles_no_output,
    const std::vector<std::string>& inputs_for_channels_text,
    const std::vector<std::string>& expected_outputs_for_channels_text,
    const std::vector<std::string>& model_memories_text,
    const std::string& inputs_for_all_channels_text,
    const std::string& expected_outputs_for_all_channels_text,
    std::string_view streaming_channel_data_suffix,
    std::string_view streaming_channel_ready_suffix,
    std::string_view streaming_channel_valid_suffix,
    std::string_view memory_read_enable_suffix,
    std::string_view memory_read_address_suffix,
    std::string_view memory_read_data_suffix,
    std::string_view memory_write_enable_suffix,
    std::string_view memory_write_address_suffix,
    std::string_view memory_write_data_suffix,
    std::string_view idle_channel_name, const int random_seed,
    const double prob_input_valid_assert, bool show_trace,
    std::string_view output_stats_path) {
  // Don't waste time and memory parsing more input than can possibly be
  // consumed.
  const int64_t total_ticks =
      std::accumulate(ticks.begin(), ticks.end(), static_cast<int64_t>(0));

  absl::flat_hash_map<std::string, std::vector<Value>> inputs_for_channels;
  if (!inputs_for_channels_text.empty()) {
    XLS_ASSIGN_OR_RETURN(
        inputs_for_channels,
        GetValuesForEachChannels(inputs_for_channels_text, total_ticks));
  } else if (!inputs_for_all_channels_text.empty()) {
    XLS_ASSIGN_OR_RETURN(
        inputs_for_channels,
        ParseChannelValuesFromFile(inputs_for_all_channels_text, total_ticks));
  }

  absl::flat_hash_map<std::string, std::vector<Value>>
      expected_outputs_for_channels;
  if (!expected_outputs_for_channels_text.empty()) {
    XLS_ASSIGN_OR_RETURN(expected_outputs_for_channels,
                         GetValuesForEachChannels(
                             expected_outputs_for_channels_text, total_ticks));
  } else if (!expected_outputs_for_all_channels_text.empty()) {
    XLS_ASSIGN_OR_RETURN(
        expected_outputs_for_channels,
        ParseChannelValuesFromFile(expected_outputs_for_all_channels_text,
                                   total_ticks));
  }

  absl::flat_hash_map<std::string, std::pair<int64_t, Value>> model_memories;
  if (!model_memories_text.empty()) {
    XLS_ASSIGN_OR_RETURN(model_memories,
                         ParseMemoryModels(model_memories_text));
  }

  XLS_ASSIGN_OR_RETURN(std::string ir_text, GetFileContents(ir_file));
  XLS_ASSIGN_OR_RETURN(auto package, Parser::ParsePackage(ir_text));

  if (backend != "block_interpreter" && !model_memories.empty()) {
    XLS_LOG(QFATAL) << "Only block interpreter supports memory models "
                       "specified to eval_proc_main";
  }

  if (backend == "serial_jit") {
    return EvaluateProcs(package.get(), /*use_jit=*/true, ticks,
                         inputs_for_channels, expected_outputs_for_channels);
  }
  if (backend == "ir_interpreter") {
    return EvaluateProcs(package.get(), /*use_jit=*/false, ticks,
                         inputs_for_channels, expected_outputs_for_channels);
  }
  if (backend == "block_interpreter") {
    verilog::ModuleSignatureProto proto;
    XLS_CHECK_OK(ParseTextProtoFile(block_signature_proto, &proto));
    return RunBlockInterpreter(
        package.get(), ticks, proto, max_cycles_no_output, inputs_for_channels,
        expected_outputs_for_channels, model_memories,
        streaming_channel_data_suffix, streaming_channel_ready_suffix,
        streaming_channel_valid_suffix, memory_read_enable_suffix,
        memory_read_address_suffix, memory_read_data_suffix,
        memory_write_enable_suffix, memory_write_address_suffix,
        memory_write_data_suffix, idle_channel_name, random_seed,
        prob_input_valid_assert, show_trace, output_stats_path);
  }
  XLS_LOG(QFATAL) << "Unknown backend type";
}

}  // namespace xls

int main(int argc, char* argv[]) {
  std::vector<std::string_view> positional_args =
      xls::InitXls(kUsage, argc, argv);
  if (positional_args.size() != 1) {
    XLS_LOG(QFATAL) << "One (and only one) IR file must be given.";
  }

  std::string backend = absl::GetFlag(FLAGS_backend);
  if (backend != "serial_jit" && backend != "ir_interpreter" &&
      backend != "block_interpreter") {
    XLS_LOG(QFATAL) << "Unrecognized backend choice.";
  }

  if (backend == "block_interpreter" &&
      absl::GetFlag(FLAGS_block_signature_proto).empty()) {
    XLS_LOG(QFATAL) << "Block interpreter requires --block_signature_proto.";
  }

  std::vector<int64_t> ticks;
  for (const std::string& run_str : absl::GetFlag(FLAGS_ticks)) {
    int ticks_int;
    if (!absl::SimpleAtoi(run_str.c_str(), &ticks_int)) {
      XLS_LOG(QFATAL) << "Couldn't parse run description in --ticks: "
                      << run_str;
    }
    ticks.push_back(ticks_int);
  }
  if (ticks.empty()) {
    XLS_LOG(QFATAL) << "--ticks must be specified.";
  }

  if (!absl::GetFlag(FLAGS_inputs_for_channels).empty() &&
      !absl::GetFlag(FLAGS_inputs_for_all_channels).empty()) {
    XLS_LOG(QFATAL) << "One of --inputs_for_channels and "
                       "--inputs_for_all_channels must be set.";
  }

  if (!absl::GetFlag(FLAGS_expected_outputs_for_channels).empty() &&
      !absl::GetFlag(FLAGS_expected_outputs_for_all_channels).empty()) {
    XLS_LOG(QFATAL) << "One of --expected_outputs_for_channels and "
                       "--expected_outputs_for_all_channels must be set.";
  }

  return xls::ExitStatus(xls::RealMain(
      positional_args[0], backend, absl::GetFlag(FLAGS_block_signature_proto),
      ticks, absl::GetFlag(FLAGS_max_cycles_no_output),
      absl::GetFlag(FLAGS_inputs_for_channels),
      absl::GetFlag(FLAGS_expected_outputs_for_channels),
      absl::GetFlag(FLAGS_model_memories),
      absl::GetFlag(FLAGS_inputs_for_all_channels),
      absl::GetFlag(FLAGS_expected_outputs_for_all_channels),
      absl::GetFlag(FLAGS_streaming_channel_data_suffix),
      absl::GetFlag(FLAGS_streaming_channel_ready_suffix),
      absl::GetFlag(FLAGS_streaming_channel_valid_suffix),
      absl::GetFlag(FLAGS_memory_read_enable_suffix),
      absl::GetFlag(FLAGS_memory_read_address_suffix),
      absl::GetFlag(FLAGS_memory_read_data_suffix),
      absl::GetFlag(FLAGS_memory_write_enable_suffix),
      absl::GetFlag(FLAGS_memory_write_address_suffix),
      absl::GetFlag(FLAGS_memory_write_data_suffix),
      absl::GetFlag(FLAGS_idle_channel_name), absl::GetFlag(FLAGS_random_seed),
      absl::GetFlag(FLAGS_prob_input_valid_assert),
      absl::GetFlag(FLAGS_show_trace), absl::GetFlag(FLAGS_output_stats_path)));
}
