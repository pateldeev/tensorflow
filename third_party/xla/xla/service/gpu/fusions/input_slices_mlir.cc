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
#include "xla/service/gpu/fusions/input_slices_mlir.h"

#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

#include "absl/algorithm/container.h"
#include "absl/log/log.h"
#include "absl/status/status.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Tensor/IR/Tensor.h"  // from @llvm-project
#include "mlir/IR/AffineExpr.h"  // from @llvm-project
#include "mlir/IR/AffineMap.h"  // from @llvm-project
#include "mlir/IR/BuiltinOps.h"  // from @llvm-project
#include "mlir/IR/ImplicitLocOpBuilder.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/IR/ValueRange.h"  // from @llvm-project
#include "xla/hlo/ir/hlo_computation.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_instructions.h"
#include "xla/service/gpu/fusions/mlir/computation_partitioner.h"
#include "xla/service/gpu/fusions/mlir/elemental_hlo_to_mlir.h"
#include "xla/service/gpu/fusions/mlir/ir/xla_gpu_ops.h"
#include "xla/service/gpu/launch_dimensions.h"
#include "xla/service/gpu/model/indexing_map.h"
#include "xla/status_macros.h"
#include "xla/xla_data.pb.h"
#include "tsl/platform/errors.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {

std::optional<IndexingMap>
MlirInputSlicesFusion::ComputeThreadIdToOutputIndexing(
    int64_t output_id, mlir::MLIRContext* ctx) const {
  // The mapping here is trivial and the same for all outputs - slice offsets
  // are applied in the indexing from slice outputs to slice inputs.
  auto launch_dims = launch_dimensions();
  // The implementation requires the shapes and layouts to be the same, but we
  // still use the requested output's shape for clarity.
  const auto& shape = analysis_.fusion_roots()[output_id]->shape();
  return GetDefaultThreadIdToOutputIndexingMap(launch_dims, unroll_factor_,
                                               shape, ctx);
}

LaunchDimensions MlirInputSlicesFusion::launch_dimensions() const {
  auto* root = analysis_.fusion_roots().front();
  const auto& shape = root->operands()[0]->shape();
  return CalculateLaunchDimensions(shape, analysis_.device_info(),
                                   {unroll_factor_});
}

absl::Status MlirInputSlicesFusion::EmitMlir(
    mlir::ModuleOp module, mlir::func::FuncOp entry_function,
    const HloFusionInstruction& fusion) const {
  mlir_converter::PartitionedComputations computations(
      fusion.fused_instructions_computation());

  const auto& root_computation = computations.FindPartitionedComputation(
      fusion.fused_instructions_computation());
  const auto& root_graph = root_computation.GetRootSubgraph();

  auto subgraph_to_mlir_fn = computations.DeclareFunctions(module);
  auto call_targets =
      computations.CreateCallTargetProvider(subgraph_to_mlir_fn);
  for (const auto& comp : computations.partitioned_computations()) {
    for (const auto& subgraph : comp.subgraphs()) {
      TF_RETURN_IF_ERROR(mlir_converter::SubgraphToMlirFunction(
          comp, subgraph, subgraph_to_mlir_fn[&subgraph], call_targets));
    }
  }

  mlir::ImplicitLocOpBuilder builder(entry_function.getLoc(), entry_function);
  builder.setInsertionPointToStart(entry_function.addEntryBlock());

  // We enforce that all the root shapes have identical dimensions in
  // IsHloOpSupported.
  auto indexing = ComputeThreadIdToOutputIndexing(0, module.getContext());
  TF_RET_CHECK(indexing) << "Indexing is never nullopt";

  int num_inputs = fusion.fused_instructions_computation()->num_parameters();
  auto output_tensor_args =
      entry_function.getArguments().drop_front(num_inputs);

  TF_ASSIGN_OR_RETURN(
      auto result_tensors,
      EmitLoopNest(
          builder, output_tensor_args, *indexing,
          [&](mlir::ValueRange output_tensors, mlir::ValueRange dim_values,
              mlir::ValueRange symbol_values)
              -> absl::StatusOr<llvm::SmallVector<mlir::Value>> {
            auto output_indices = mlir_converter::ApplyAffineMap(
                indexing->GetAffineMap(), dim_values, symbol_values, builder);
            auto root_fn = subgraph_to_mlir_fn[&root_graph];

            llvm::SmallVector<mlir::Value> operands(
                entry_function.getArguments().take_front(num_inputs));
            absl::c_copy(output_indices, std::back_inserter(operands));

            auto result_scalars =
                builder.create<PureCallOp>(root_fn, operands).getResults();

            llvm::SmallVector<mlir::Value> result_tensors;
            result_tensors.reserve(output_tensor_args.size());
            for (auto [tensor, value] :
                 llvm::zip(output_tensors, result_scalars)) {
              result_tensors.push_back(builder
                                           .create<mlir::tensor::InsertOp>(
                                               value, tensor, output_indices)
                                           .getResult());
            }
            return result_tensors;
          }));
  builder.create<mlir::func::ReturnOp>(result_tensors);

  return absl::OkStatus();
}

}  // namespace gpu
}  // namespace xla
