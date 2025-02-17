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

#include <memory>

#include <gtest/gtest.h>
#include "absl/status/statusor.h"
#include "xla/error_spec.h"
#include "xla/service/gpu/fusions/mlir/mlir_fusion_emitter.h"
#include "xla/service/gpu/fusions/mlir_emitter_test_base.h"
#include "xla/service/gpu/hlo_fusion_analysis.h"
#include "xla/tests/filecheck.h"
#include "tsl/platform/statusor.h"

namespace xla {
namespace gpu {
namespace {

class MlirInputSlicesFusionTest : public MlirEmitterTestBase {
 public:
  std::unique_ptr<MlirFusionEmitterBase> GetEmitter(
      const HloFusionAnalysis& analysis) override {
    return std::make_unique<MlirInputSlicesFusion>(analysis);
  }
};

TEST_F(MlirInputSlicesFusionTest, SimpleInputSlices) {
  auto kHloString = R"(
HloModule module

fused_computation {
  %input = f32[2,3,5,7]{2,1,0,3} parameter(0)
  slice0 = f32[1,2,3,5]{2,1,0,3} slice(input), slice={[0:1],[1:3],[0:3],[2:7]}
  slice1 = f32[1,2,3,5]{2,1,0,3} slice(input), slice={[0:1],[0:2],[0:3],[2:7]}
  ROOT tuple = (f32[1,2,3,5]{2,1,0,3}, f32[1,2,3,5]{2,1,0,3}) tuple(slice0, slice1)
}

ENTRY entry {
  %input = f32[2,3,5,7]{2,1,0,3} parameter(0)
  ROOT %fusion = (f32[1,2,3,5]{2,1,0,3}, f32[1,2,3,5]{2,1,0,3}) fusion(%input), kind=kLoop, calls=fused_computation
})";

  TF_ASSERT_OK_AND_ASSIGN(auto ir, EmitIR(kHloString));

  ASSERT_TRUE(RunFileCheck(ir, R"(
// CHECK: arith.cmpi sge
// CHECK: arith.cmpi sle
// CHECK: arith.andi
// CHECK: scf.if
// CHECK: func.func private @fused_computation_tuple
// CHECK-COUNT-2: tensor.extract
)")
                  .value());

  EXPECT_TRUE(RunAndCompareNoHloPasses(kHloString, ErrorSpec{1e-3}));
}

}  // namespace
}  // namespace gpu
}  // namespace xla
