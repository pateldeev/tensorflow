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
#ifndef XLA_SERVICE_GPU_FUSIONS_MLIR_ELEMENTAL_HLO_TO_MLIR_H_
#define XLA_SERVICE_GPU_FUSIONS_MLIR_ELEMENTAL_HLO_TO_MLIR_H_

#include <functional>

#include "absl/status/status.h"
#include "absl/status/statusor.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/service/gpu/fusions/mlir/computation_partitioner.h"
#include "xla/service/gpu/hlo_traversal.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "xla/stream_executor/device_description.h"

namespace xla {
namespace gpu {
namespace mlir_converter {

using OperandProvider =
    std::function<absl::StatusOr<llvm::SmallVector<mlir::Value>>(
        const HloInstruction* instr, int index, mlir::ValueRange indices)>;

// Emits MLIR to generate the given element of the HLO instruction. Required
// operands are accessed through the `operand_provider` function.
// CHECK fails if IsHloConversionSupported returns false.
absl::StatusOr<llvm::SmallVector<mlir::Value>> HloToMlir(
    const HloInstruction* instr, mlir::ValueRange indices,
    const OperandProvider& operand_provider,
    const CallTargetProvider& call_target_provider,
    mlir::ImplicitLocOpBuilder& builder);

// Emits MLIR to produce the value(s) of a parameter. The parameter must be
// located outside the subgraph.
absl::StatusOr<llvm::SmallVector<mlir::Value>> ProvideParameter(
    const PartitionedComputation& computation, const HloInstruction* instr,
    int operand_index, mlir::ValueRange indices,
    const CallTargetProvider& call_target_provider,
    mlir::ImplicitLocOpBuilder& builder);

// Checks whether the given HLO instruction can be converted to MLIR.
bool IsHloOpSupported(const HloInstruction* instr,
                      se::CudaComputeCapability compute_capability);

// Checks whether the given HLO computation is supported by the MLIR converter:
// - all instructions in it are supported
// - the signature is supported: if the computation is not a fusion computation,
//   all arguments have rank 0.
bool IsHloConversionSupported(const HloComputation* computation,
                              se::GpuComputeCapability compute_capability);
bool IsHloConversionSupported(const HloFusionAdaptor& fusion,
                              se::GpuComputeCapability compute_capability);

// Converts a function (subgraph) to an MLIR function producing one element of
// the result. The function must have the correct interface.
absl::Status SubgraphToMlirFunction(
    const PartitionedComputation& computation,
    const PartitionedComputation::Subgraph& subgraph, mlir::func::FuncOp& func,
    const CallTargetProvider& call_target_provider);

// Creates an affine.apply op for the given expression and values.
mlir::Value ApplyAffineExpr(mlir::AffineExpr expr, mlir::ValueRange dims,
                            mlir::ValueRange symbols,
                            mlir::ImplicitLocOpBuilder& b);

// Creates affine.apply ops for each result of the given map.
llvm::SmallVector<mlir::Value> ApplyAffineMap(mlir::AffineMap map,
                                              mlir::ValueRange dims,
                                              mlir::ValueRange symbols,
                                              mlir::ImplicitLocOpBuilder& b);

// Checks all the **constraints** in the map (not the **ranges**).
mlir::Value CheckConstraints(const IndexingMap& map, mlir::ValueRange dims,
                             mlir::ValueRange symbols,
                             mlir::ImplicitLocOpBuilder& b);

}  // namespace mlir_converter
}  // namespace gpu
}  // namespace xla

#endif  // XLA_SERVICE_GPU_FUSIONS_MLIR_ELEMENTAL_HLO_TO_MLIR_H_
