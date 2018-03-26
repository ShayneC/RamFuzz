// Copyright 2016-2018 The RamFuzz contributors. All rights reserved.
//
// Licensed under the Apache License, Version 2.0 (the "License");
// you may not use this file except in compliance with the License.
// You may obtain a copy of the License at
//
//     http://www.apache.org/licenses/LICENSE-2.0
//
// Unless required by applicable law or agreed to in writing, software
// distributed under the License is distributed on an "AS IS" BASIS,
// WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
// See the License for the specific language governing permissions and
// limitations under the License.

#include "fuzz.hpp"

using namespace ramfuzz;
using namespace runtime;

int main(int argc, char *argv[]) {
  gen g(argc, argv);
  while (g.make<Base<int>>(101)->f() != 0xba)
    ;
  // A's only subclass is a template, which is ignored.  Therefore, its subcount
  // should be 0.
  return harness<A>::subcount;
}

unsigned ::ramfuzz::runtime::spinlimit = 3;
