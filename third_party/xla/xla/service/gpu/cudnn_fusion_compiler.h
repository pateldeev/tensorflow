/* Copyright 2024 The TensorFlow Authors. All Rights Reserved.

Licensed under the Apache License, Version 2.0 (the "License");
you may not use this file except in compliance with the License.
You may obtain a copy of the License at

    http://www.apache.org/licenses/LICENSE-2.0

Unless required by applicable law or agreed to in writing, software
distributed under the License is distributed on an "AS IS" BASIS,
WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
See the License for the specific language governing permissions and
limitations under the License.
==============================================================================*/

#ifndef XLA_SERVICE_GPU_CUDNN_FUSION_COMPILER_H_
#define XLA_SERVICE_GPU_CUDNN_FUSION_COMPILER_H_

#include "absl/container/flat_hash_set.h"
#include "absl/strings/string_view.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/gpu/autotuner_util.h"
#include "xla/service/hlo_pass_interface.h"

namespace xla {
namespace gpu {

// Converts HLO fusions with cuDNN backend config to cuDNN graphs,
// compiles them using a cuDNN handle and stores them in the
// backend config in serialized form.
class CuDnnFusionCompiler : public HloModulePass {
 public:
  explicit CuDnnFusionCompiler(const AutotuneConfig& config)
      : config_(config) {}

  absl::string_view name() const override { return "cudnn-fusion-compiler"; }

  using HloPassInterface::Run;
  absl::StatusOr<bool> Run(
      HloModule* module,
      const absl::flat_hash_set<absl::string_view>& execution_threads) override;

 private:
  AutotuneConfig config_;
};

}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_CUDNN_FUSION_COMPILER_H_
