/* Copyright 2023 The OpenXLA Authors.

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

#ifndef XLA_SERVICE_GPU_RUNTIME_ANNOTATION_H_
#define XLA_SERVICE_GPU_RUNTIME_ANNOTATION_H_

#include <optional>
#include <string>
#include <string_view>

#include "absl/container/flat_hash_map.h"
#include "xla/hlo/ir/hlo_instruction.h"
#include "xla/hlo/ir/hlo_module.h"
#include "tsl/profiler/lib/nvtx_utils.h"
#include "tsl/profiler/lib/scoped_annotation.h"

namespace xla::gpu {

// Prepared information for the top level NVTX/profiler range covering an
// HloModule
class ModuleAnnotation {
 public:
  explicit ModuleAnnotation(std::string_view module_name);
  explicit ModuleAnnotation(const HloModule& mod);

  std::string_view longest_op_name_prefix() const { return longest_prefix_; }
  explicit operator std::string_view() const { return title_str_; }
  nvtxStringHandle_t title() const { return title_; }
  static uint64_t NvtxSchemaId();
  int32_t common_stack_frames() const { return common_stack_frames_; }

 private:
  friend void RangePush(nvtxDomainHandle_t domain,
                        const ModuleAnnotation& annotation) {
    tsl::profiler::RangePush(domain, annotation.title(), annotation);
  }

  std::string longest_prefix_;
  std::string title_str_;
  nvtxStringHandle_t title_;
  nvtxStringHandle_t module_name_;
  nvtxStringHandle_t common_src_locations_{};
  int32_t module_id_{-1};
  int32_t common_stack_frames_{};
};

// Prepared information for a kernel/thunk/fusion/... within an HloModule
struct KernelAnnotation {
  KernelAnnotation(const ModuleAnnotation& module_annotation,
                   const HloInstruction& inst);

  explicit operator std::string_view() const { return title_str; }
  static uint64_t NvtxSchemaId();

 private:
  friend void RangePush(nvtxDomainHandle_t domain,
                        const KernelAnnotation& annotation) {
    tsl::profiler::RangePush(domain, annotation.title, annotation);
  }

  std::string title_str;
  nvtxStringHandle_t title;
  nvtxStringHandle_t hlo_dump;
  nvtxStringHandle_t src_locations;
  nvtxStringHandle_t called_hlo_dump;
};

// Parsed/prepared information for an HloModule that gets propagated to NVTX
// ranges/profilers/... at execution time.
struct ModuleAnnotations {
  explicit ModuleAnnotations(std::string_view module_name);
  explicit ModuleAnnotations(const HloModule&);

  ModuleAnnotation top_level;
  absl::flat_hash_map<std::string_view, KernelAnnotation> kernels;
};

//===----------------------------------------------------------------------===//
// Scoped RAII helper to set and restore thread local module annotations
//===----------------------------------------------------------------------===//

class ScopedModuleAnnotations {
 public:
  explicit ScopedModuleAnnotations(const ModuleAnnotations* annotations);
  ~ScopedModuleAnnotations();

 private:
  const ModuleAnnotations* restore_;
};

const ModuleAnnotations* GetCurrentModuleAnnotations();

std::optional<tsl::profiler::ScopedAnnotation> GetKernelAnnotation(
    const ModuleAnnotations* annotations, std::string_view profile_annotation);

}  // namespace xla::gpu

#endif  // XLA_SERVICE_GPU_RUNTIME_ANNOTATION_H_
