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

#ifndef XLS_IR_INTERVAL_H_
#define XLS_IR_INTERVAL_H_

#include "absl/status/statusor.h"
#include "absl/strings/str_format.h"
#include "absl/types/optional.h"
#include "xls/ir/bits.h"
#include "xls/ir/bits_ops.h"

namespace xls {

// This is a type representing intervals in the set of `Bits` of a given
// bit width. It allows improper intervals (i.e.: ones where the lower bound
// is greater than the upper bound, so the interval wraps around the end),
// though some methods do not support them (and check to ensure that they are
// not called on them). Intervals of bit width 0 are disallowed, and are used
// as a sentinel to check for accidentally calling methods on default
// constructed intervals.
class Interval {
 public:
  // No argument constructor for `Interval`. This returns the interval from a
  // zero-bit 0 to another zero-bit 0. All methods in this class have XLS_CHECK
  // calls that ensure that they are not called on an interval of bitwidth 0.
  Interval() {}

  // Create an `Interval`. The `bit_count()` of the lower bound must be equal to
  // that of the upper bound. The `bit_count()` of both bounds must be greater
  // than zero.
  //
  // The upper/lower bound are both considered inclusive.
  Interval(const Bits& lower_bound, const Bits& upper_bound)
      : lower_bound_(lower_bound), upper_bound_(upper_bound) {
    XLS_CHECK_EQ(lower_bound_.bit_count(), upper_bound_.bit_count());
    XLS_CHECK_GT(lower_bound_.bit_count(), 0);
  }

  // The inclusive lower bound of the interval.
  const Bits& LowerBound() const { return lower_bound_; }

  // The inclusive upper bound of the interval.
  const Bits& UpperBound() const { return upper_bound_; }

  // Returns the number of bits in the lower/upper bound of the interval
  int64_t BitCount() const;

  // Returns an `Interval` that covers every bit pattern of a given width.
  static Interval Maximal(int64_t bit_width);

  // Given two `Interval`s, return whether they overlap.
  // Does not accept improper intervals.
  static bool Overlaps(const Interval& lhs, const Interval& rhs);

  // Given two `Interval`s, return whether they are disjoint.
  // Does not accept improper intervals.
  static bool Disjoint(const Interval& lhs, const Interval& rhs);

  // Interval (a, b) "abuts" interval (x, y) if b + 1 = x or y + 1 = a
  // In other words, they abut iff they do not overlap but their union is itself
  // an interval.
  // For example, (5, 7) and (8, 12) do not overlap but their union is (5, 12).
  // Does not accept improper intervals.
  static bool Abuts(const Interval& lhs, const Interval& rhs);

  // Given two `Interval`s, return an `Interval` representing their convex hull.
  // Does not accept improper intervals.
  static Interval ConvexHull(const Interval& lhs, const Interval& rhs);

  // Iterate over every point in the interval, calling the given callback for
  // each point. If the callback returns `true`, terminate the iteration early
  // and return `true`. Otherwise, continue the iteration until all points have
  // been visited and return `false`.
  bool ForEachElement(std::function<bool(const Bits&)> callback) const;

  // This is similar to `ForEachElement`, except it accumulates the result
  // into a `std::vector<Bits>` instead of using a callback. This is often
  // impractical as it will use a lot of memory, but can be useful temporarily
  // for debugging.
  std::vector<Bits> Elements() const;

  // Returns the number of points contained within the interval as a `Bits`.
  //
  // The returned `Bits` has a bitwidth that is one greater than
  // the `BitCount()` of this interval.
  Bits SizeBits() const;

  // Returns the number of points contained within the interval, assuming that
  // number fits within a `uint64_t`. If it doesn't, `absl::nullopt` is
  // returned.
  absl::optional<int64_t> Size() const;

  // Returns `true` if this is an improper interval, `false` otherwise.
  // An improper interval is one where the upper bound is strictly less than
  // the lower bound.
  bool IsImproper() const;

  // Returns `true` if this is a precise interval, `false` otherwise.
  // A precise interval is one that covers exactly one point.
  bool IsPrecise() const;

  // Returns `true` if this is a maximal interval, `false` otherwise.
  // A maximal interval is one that covers every point of a given bitwidth.
  bool IsMaximal() const;

  // Returns `true` if this interval covers the given point, `false` otherwise.
  bool Covers(const Bits& point) const;

  // Returns `true` if this interval covers zero, `false` otherwise.
  bool CoversZero() const;

  // Returns `true` if this interval covers one, `false` otherwise.
  bool CoversOne() const;

  // Returns `true` if this interval covers `Bits::AllOnes(this->BitCount())`,
  // `false` otherwise.
  bool CoversMax() const;

  // Prints the interval as a string.
  std::string ToString() const;

  // Lexicographic ordering of intervals.
  friend bool operator<(const Interval& lhs, const Interval& rhs) {
    if (bits_ops::ULessThan(lhs.lower_bound_, rhs.lower_bound_)) {
      return true;
    } else if (bits_ops::UEqual(lhs.lower_bound_, rhs.lower_bound_)) {
      return bits_ops::ULessThan(lhs.upper_bound_, rhs.upper_bound_);
    }
    return false;
  }

  // Equality of intervals.
  friend bool operator==(const Interval& lhs, const Interval& rhs) {
    if (bits_ops::UEqual(lhs.lower_bound_, rhs.lower_bound_) &&
        bits_ops::UEqual(lhs.upper_bound_, rhs.upper_bound_)) {
      return true;
    }
    return false;
  }

 private:
  void EnsureValid() const;

  Bits lower_bound_;
  Bits upper_bound_;
};

}  // namespace xls

#endif  // XLS_IR_INTERVAL_H_
