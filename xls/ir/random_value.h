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

#ifndef XLS_IR_RANDOM_VALUE_H_
#define XLS_IR_RANDOM_VALUE_H_

#include <random>
#include <vector>

#include "xls/ir/function.h"
#include "xls/ir/type.h"
#include "xls/ir/value.h"

namespace xls {

// Returns a Value with random uniformly distributed bits using the given
// engine.
Value RandomValue(Type* type, std::minstd_rand* engine);

// Returns a set of argument values for the given function with random uniformly
// distributed bits using the given engine.
std::vector<Value> RandomFunctionArguments(Function* f,
                                           std::minstd_rand* engine);

}  // namespace xls

#endif  // XLS_IR_RANDOM_VALUE_H_
