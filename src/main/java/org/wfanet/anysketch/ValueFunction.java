// Copyright 2020 The Any Sketch Authors
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

package org.wfanet.anysketch;

/**
 * ValueFunction is a commutative  binary function that takes two values and combines them into one.
 * Addition is the canonical example.
 */
interface ValueFunction {

  /**
   * Returns the String name to find item in the map
   */
  String name();

  /**
   * Returns the new value by computing old and new value related to the algorithm
   */
  long getValue(long old_value, long new_value);

  /**
   * Returns the initial value before any computation
   */
  long getInitialValue(long new_value);
}