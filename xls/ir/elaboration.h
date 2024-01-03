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

#ifndef XLS_IR_ELABORATION_H_
#define XLS_IR_ELABORATION_H_

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "absl/container/flat_hash_map.h"
#include "absl/container/flat_hash_set.h"
#include "absl/status/statusor.h"
#include "absl/types/span.h"
#include "xls/ir/channel.h"
#include "xls/ir/package.h"
#include "xls/ir/proc.h"
#include "xls/ir/proc_instantiation.h"

namespace xls {

// Library for elaborating a proc hierarchy. A proc hierarchy is a directed
// acyclic graph of procs connected via proc instantiation. An elaboration
// flattens the proc hierarchy into a tree by walking all paths in the hierarchy
// starting at a `top` proc where a path is a chain of proc instantiations. For
// each IR construct (proc or channel), The elaboration creates a separate
// "instance" object for each path through the hierarchy from the top proc to
// the IR construct.
//
// Example proc hierarchy:
//
//   proc leaf_proc<ch0: ... in, ch0: .... out>(...) { }
//
//   proc other_proc<x: ... in, y: .... out>(...) {
//     chan z(...)
//     proc_instantiation other_inst0(x, z, proc=leaf_proc)
//     proc_instantiation other_inst1(z, y, proc=leaf_proc)
//   }
//
//   proc my_top<a: ... in, b: ... out>(...) {
//     chan c(...)
//     chan d(...)
//     proc_instantiation my_inst0(a, b, proc=other_proc)
//     proc_instantiation my_inst1(c, c, proc=other_proc)
//     proc_instantiation my_inst2(d, d, proc=leaf_proc)
//   }
//
// Elaborating this hierarchy from `my_top` yields the following elaboration
// tree. Each line is a instance of either a proc or a channel.
//
//  my_top
//    chan c
//    chan d
//    other_proc<a, b> [my_inst0]
//      chan z
//      leaf_proc<x, z> [other_inst0]
//      leaf_proc<z, y> [other_inst1]
//    other_proc<c, c> [my_inst1]
//      chan z
//      leaf_proc<x, z> [other_inst0]
//      leaf_proc<z, y> [other_inst1]
//    leaf_proc<d, d> [my_inst2]
//
// There are five instances of `leaf_proc` as there are five paths from
// `top_proc` to `leaf_proc` in the proc hierarchy.

// A path of proc instantiations. An instance of a proc or channel is uniquely
// identified by its InstantiationPath.
struct InstantiationPath {
  Proc* top;
  std::vector<ProcInstantiation*> path;

  template <typename H>
  friend H AbslHashValue(H h, const InstantiationPath& p) {
    H state = H::combine(std::move(h), p.top->name());
    for (const ProcInstantiation* element : p.path) {
      state = H::combine(std::move(state), element->name());
    }
    return state;
  }
  bool operator==(const InstantiationPath& other) const {
    return top == other.top && path == other.path;
  }
  bool operator!=(const InstantiationPath& other) const {
    return !(*this == other);
  }

  std::string ToString() const;
};

struct ChannelInstance {
  Channel* channel;

  // Instantiation path of the proc instance in which this channel is
  // defined. Is nullopt for old-style channels.
  std::optional<InstantiationPath> path;

  std::string ToString() const;
};

// Representation of an instance of a proc. This is a recursive data structure
// which also holds all channel and proc instances instantiated by this proc
// instance including recursively.
class ProcInstance {
 public:
  ProcInstance(Proc* proc, std::optional<ProcInstantiation*> proc_instantiation,
               std::optional<InstantiationPath> path,
               absl::Span<ChannelInstance* const> interface,
               std::vector<std::unique_ptr<ChannelInstance>> channel_instances,
               std::vector<std::unique_ptr<ProcInstance>> instantiated_procs)
      : proc_(proc),
        proc_instantiation_(proc_instantiation),
        path_(std::move(path)),
        interface_(interface.begin(), interface.end()),
        channels_(std::move(channel_instances)),
        instantiated_procs_(std::move(instantiated_procs)) {}

  Proc* proc() const { return proc_; }

  // The ProcInstantiation IR construct which instantiates this proc
  // instance. This is nullopt if the proc corresponding to this ProcInstance
  // is the top proc.
  std::optional<ProcInstantiation*> proc_instantiation() const {
    return proc_instantiation_;
  }

  // The path to this proc instance through the proc hierarchy. This is nullopt
  // for old-style procs.
  const std::optional<InstantiationPath>& path() const { return path_; }

  // The ChannelInstances comprising the interface of this proc instance.
  absl::Span<ChannelInstance* const> interface() const { return interface_; }

  // The ChannelInstances corresponding to the channels declared in the proc
  // associated with this proc instance.
  absl::Span<const std::unique_ptr<ChannelInstance>> channels() const {
    return channels_;
  }

  // The ProcInstances instantiated by this proc instance.
  absl::Span<const std::unique_ptr<ProcInstance>> instantiated_procs() const {
    return instantiated_procs_;
  }

  // Returns the ChannelInstance with the given name in this proc instance. The
  // channel instance can refer to an interface channel or a channel defined in
  // the proc.
  absl::StatusOr<ChannelInstance*> GetChannelInstance(
      std::string_view channel_name) const;

  // Returns a unique name for this proc instantiation. For new-style procs this
  // includes the proc name and the instantiation path. For old-style procs this
  // is simply the proc name.
  std::string GetName() const;

  // Return a nested representation of the proc instance.
  std::string ToString(int64_t indent_amount = 0) const;

 private:
  Proc* proc_;
  std::optional<ProcInstantiation*> proc_instantiation_;
  std::optional<InstantiationPath> path_;
  std::vector<ChannelInstance*> interface_;

  // Channel and proc instances in this proc instance. Unique pointers are used
  // for pointer stability as pointers to these objects are handed out.
  std::vector<std::unique_ptr<ChannelInstance>> channels_;
  std::vector<std::unique_ptr<ProcInstance>> instantiated_procs_;
};

// Data structure representing the elaboration tree.
class Elaboration {
 public:
  static absl::StatusOr<Elaboration> Elaborate(Proc* top);

  // Elaborate the package of old style procs. This generates a single instance
  // for each proc and channel in the package. The instance paths of each object
  // are std::nullopt.

  // TODO(https://github.com/google/xls/issues/869): Remove when all procs are
  // new style.
  static absl::StatusOr<Elaboration> ElaborateOldStylePackage(Package* package);

  ProcInstance* top() const { return top_.get(); }

  std::string ToString() const;

  // Returns the proc/channel instance at the given path.
  absl::StatusOr<ProcInstance*> GetProcInstance(
      const InstantiationPath& path) const;
  absl::StatusOr<ChannelInstance*> GetChannelInstance(
      std::string_view channel_name, const InstantiationPath& path) const;

  // Returns the proc/channel instance at the given path where the path is given
  // as a serialization (e.g., `top_proc::inst->other_proc`).
  absl::StatusOr<ProcInstance*> GetProcInstance(
      std::string_view path_str) const;
  absl::StatusOr<ChannelInstance*> GetChannelInstance(
      std::string_view channel_name, std::string_view path_str) const;

  // Return a vector of all proc or channel instances in the elaboration.
  absl::Span<ProcInstance* const> proc_instances() const {
    return proc_instance_ptrs_;
  }
  absl::Span<ChannelInstance* const> channel_instances() const {
    return channel_instance_ptrs_;
  }

  // Return all instances of a particular channel/proc.
  absl::Span<ProcInstance* const> GetInstances(Proc* proc) const;
  absl::Span<ChannelInstance* const> GetInstances(Channel* channel) const;

  // Return all channel instances which the given channel reference is bound to
  // in the elaboration.
  absl::Span<ChannelInstance* const> GetInstancesOfChannelReference(
      ChannelReference* channel_reference) const;

  // Return the unique instance of the given proc/channel. Returns an error if
  // there is not exactly one instance associated with the IR object.
  absl::StatusOr<ProcInstance*> GetUniqueInstance(Proc* proc) const;
  absl::StatusOr<ChannelInstance*> GetUniqueInstance(Channel* channel) const;

  Package* package() const { return package_; }

  // Create path from the given path string serialization. Example input:
  //
  //    top_proc::inst1->other_proc::inst2->that_proc
  //
  // The return path will have the Proc pointer to `top_proc` as the top of the
  // path, with an instantiation path containing the ProcInstantiation pointers:
  // {inst1, inst2}.
  //
  // Returns an error if the path does not exist in the elaboration.
  absl::StatusOr<InstantiationPath> CreatePath(std::string_view path_str) const;

 private:
  // Walks the hierarchy and builds the data member maps of instances.  Only
  // should be called for new-style procs.
  void BuildInstanceMaps(ProcInstance* proc_instance);

  Package* package_;

  // For a new style procs this is the top-level instantiation. All other
  // ProcInstances are contained within this instance.
  std::unique_ptr<ProcInstance> top_;

  // For non-new-style procs, this is the list of proc/channel instantiations,
  // one per proc in the package.
  std::vector<std::unique_ptr<ProcInstance>> proc_instances_;
  std::vector<std::unique_ptr<ChannelInstance>> channel_instances_;

  // Vectors of all proc/channel instances in the elaboration.
  std::vector<ProcInstance*> proc_instance_ptrs_;
  std::vector<ChannelInstance*> channel_instance_ptrs_;

  // Channel object for the interface of the top-level proc. This is necessary
  // as there are no associated Channel objects in the IR.
  // TODO(https://github.com/google/xls/issues/869): An IR object should
  // probably not live outside the IR. Distill the necessary information from
  // Channel and use that instead.
  std::vector<std::unique_ptr<Channel>> interface_channels_;

  // Channel instances for the interface channels.
  std::vector<std::unique_ptr<ChannelInstance>> interface_channel_instances_;

  // All proc instances in the elaboration indexed by instantiation path.
  absl::flat_hash_map<InstantiationPath, ProcInstance*> proc_instances_by_path_;

  // All channel instances in the elaboration indexed by channel name and
  // instantiation path.
  absl::flat_hash_map<std::pair<std::string, InstantiationPath>,
                      ChannelInstance*>
      channel_instances_by_path_;

  // List of instances of each Proc/Channel.
  absl::flat_hash_map<Proc*, std::vector<ProcInstance*>> instances_of_proc_;
  absl::flat_hash_map<Channel*, std::vector<ChannelInstance*>>
      instances_of_channel_;

  // List of channel instances for each channel reference.
  absl::flat_hash_map<ChannelReference*, std::vector<ChannelInstance*>>
      instances_of_channel_reference_;
};

}  // namespace xls

#endif  // XLS_IR_ELABORATION_H_
