/* Copyright 2024 The OpenXLA Authors.

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
#ifndef XLA_SERVICE_GPU_FUSIONS_INPUT_SLICES_MLIR_H_
#define XLA_SERVICE_GPU_FUSIONS_INPUT_SLICES_MLIR_H_

#include <cstdint>
#include <optional>

#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/Interfaces/DataLayoutInterfaces.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/gpu/fusions/mlir/mlir_fusion_emitter.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/status.h"

namespace xla {
namespace gpu {

// Generates code for input-fusible slices. Lowers to LLVM via MLIR.
class MlirInputSlicesFusion : public MlirFusionEmitterBase {
 public:
  explicit MlirInputSlicesFusion(const HloFusionAnalysis& analysis)
      : analysis_(analysis),
        unroll_factor_(analysis.input_output_info().has_4_bit_output ? 2 : 1) {}
  LaunchDimensions launch_dimensions() const override;

  std::optional<IndexingMap> ComputeThreadIdToOutputIndexing(
      int64_t output_id, mlir::MLIRContext* ctx) const override;

  std::optional<IndexingMap> ComputeThreadIdToInputIndexing(
      int64_t root_index, int64_t hero_operand_index,
      mlir::MLIRContext* ctx) const override {
    // TODO(b/319081342): Implement this.
    return std::nullopt;
  }

 protected:
  absl::Status EmitMlir(mlir::ModuleOp module,
                        mlir::func::FuncOp entry_function,
                        const HloFusionInstruction& fusion) const override;

 private:
  const HloFusionAnalysis& analysis_;
  const int unroll_factor_;
};

}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_FUSIONS_INPUT_SLICES_MLIR_H_
