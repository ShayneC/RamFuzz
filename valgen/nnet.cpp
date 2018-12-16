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

#include "nnet.hpp"

#include <torch/torch.h>
#include <unistd.h>
#include <iostream>

#include "dataset.hpp"

using namespace std;

namespace {

/// Logistic function designed to squish input values from their C++ range
/// (typically maximal type range from std::numeric_limits) to a much smaller
/// range suitable for valgen_nnet layers to process.
torch::Tensor squish(const torch::Tensor& x) {
  const auto k = .2, L = 10.;
  return (torch::ones_like(x) + (-k * x).exp()).reciprocal() * L;
}

}  // namespace

namespace ramfuzz {

valgen_nnet::valgen_nnet() : Module("ValgenNet"), lin(nullptr) {
  lin = register_module("lin1", torch::nn::Linear(10, 2));

  // Need as large a range as possible for input values, which come from
  // arbitrary C++ programs:
  to(at::kDouble);
}

torch::Tensor valgen_nnet::forward(const torch::Tensor& vals) {
  return torch::softmax(lin->forward(squish(vals)), 0);
}

void valgen_nnet::train_more(const exetree::node& root) {
  train();
  // Batch gradient descent implementation based on this suggestion:
  // https://discuss.pytorch.org/t/how-to-process-large-batches-of-data/6740/4
  auto opt = torch::optim::Adagrad(parameters(), 0.1);
  opt.zero_grad();
  size_t data_count = 0, success_count = 0;
  for (exetree::dfs_cursor current(root); current; ++current) {
    const auto values = last_n(&*current, 10);
    const auto pred = forward(values);
    const bool mw = current->dst()->maywin();
    const auto target = bool_as_prediction(mw);
    torch::soft_margin_loss(pred, target).backward();
    if (prediction_as_bool(pred) == mw) ++success_count;
    ++data_count;
  }
  static const char line_reset = isatty(STDOUT_FILENO) ? '\r' : '\n';
  cout << "valgen_nnet accuracy: " << fixed << setprecision(4)
       << float(success_count) / data_count << line_reset << flush;
  opt.step();
  eval();
}

}  // namespace ramfuzz