/* Copyright 2019 The TensorFlow Authors. All Rights Reserved.

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

#include "tensorflow/compiler/mlir/lite/quantization/quantization_driver.h"

#include <cmath>
#include <cstdint>
#include <iterator>
#include <limits>
#include <memory>
#include <utility>
#include <vector>

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/STLExtras.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "mlir/Dialect/Arith/IR/Arith.h"  // from @llvm-project
#include "mlir/Dialect/Func/IR/FuncOps.h"  // from @llvm-project
#include "mlir/Dialect/Quant/QuantTypes.h"  // from @llvm-project
#include "mlir/IR/Attributes.h"  // from @llvm-project
#include "mlir/IR/Builders.h"  // from @llvm-project
#include "mlir/IR/BuiltinAttributes.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypeInterfaces.h"  // from @llvm-project
#include "mlir/IR/BuiltinTypes.h"  // from @llvm-project
#include "mlir/IR/MLIRContext.h"  // from @llvm-project
#include "mlir/IR/Matchers.h"  // from @llvm-project
#include "mlir/IR/OpDefinition.h"  // from @llvm-project
#include "mlir/IR/Operation.h"  // from @llvm-project
#include "mlir/IR/Value.h"  // from @llvm-project
#include "mlir/Support/LLVM.h"  // from @llvm-project
#include "tensorflow/compiler/mlir/lite/quantization/ir/QuantOps.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_traits.h"
#include "tensorflow/compiler/mlir/lite/quantization/quantization_utils.h"

namespace mlir {
namespace quant {

namespace {
// This is used to identify an operand or result of an op. The second element
// of this pair is the index of the operand or result.
using OpValue = std::pair<mlir::Operation*, int>;

// Uses the type of `value` to set the initial state of the index-th result if
// `as_result` is true or index-th operand if `as_result` is false. The state
// is immutable if the type is a quantized type. Returns the index of this
// new state in the state vector.
void InitializeStateForValue(Operation* op, const int index, const Value value,
                             const bool as_result,
                             std::vector<QuantState>* states,
                             llvm::DenseMap<Value, int>* value_to_state,
                             llvm::DenseMap<OpValue, int>* operand_states,
                             llvm::DenseMap<OpValue, int>* result_states) {
  const auto [cached, inserted] = value_to_state->insert({value, 0});
  if (!inserted) {
    if (as_result)
      (*result_states)[{op, index}] = cached->second;
    else
      (*operand_states)[{op, index}] = cached->second;
    return;
  }
  const QuantParams params =
      quant::QuantizedType::getQuantizedElementType(value.getType());
  const bool immutable = !HasQuantParams(params);
  const int next_state_index = states->size();
  states->push_back({params, immutable});
  if (as_result)
    (*result_states)[{op, index}] = next_state_index;
  else
    (*operand_states)[{op, index}] = next_state_index;
  cached->second = next_state_index;
}

}  // namespace

void QuantizationDriver::InitializeArgState(const BlockArgument arg,
                                            const Value arg_value) {
  const auto [cached, inserted] = value_to_state_.insert({arg_value, 0});
  if (!inserted) {
    arg_states_[arg] = cached->second;
    return;
  }
  const QuantParams params =
      quant::QuantizedType::getQuantizedElementType(arg_value.getType());
  const bool immutable = !HasQuantParams(params);
  const int next_state_index = states_.size();
  states_.push_back({params, immutable});
  arg_states_[arg] = next_state_index;
  cached->second = next_state_index;
}

void QuantizationDriver::InitializeOperandState(Operation* op, const int index,
                                                const Value value) {
  ::mlir::quant::InitializeStateForValue(op, index, value, /*as_result=*/false,
                                         &states_, &value_to_state_,
                                         &operand_states_, &result_states_);
}

void QuantizationDriver::InitializeResultState(Operation* op, const int index,
                                               const Value value) {
  ::mlir::quant::InitializeStateForValue(op, index, value, /*as_result=*/true,
                                         &states_, &value_to_state_,
                                         &operand_states_, &result_states_);
}

std::unique_ptr<OpQuantSpec> QuantizationDriver::GetQuantSpec(Operation* op) {
  return op_quant_spec_getter_(op);
}

std::unique_ptr<OpQuantScaleSpec> QuantizationDriver::GetQuantScaleSpec(
    Operation* op) {
  return op_quant_scale_spec_getter_(op);
}

bool QuantizationDriver::IsQuantized(Operation* op) {
  for (int i = 0; i < op->getNumResults(); ++i) {
    if (GetResultQuantState(op, i).IsEmpty()) return false;
  }
  return true;
}

bool QuantizationDriver::SetConstantResultParams(Operation* op) {
  DenseFPElementsAttr attr;
  const Value res = op->getResult(0);
  if (!matchPattern(res, m_Constant(&attr))) {
    return false;
  }
  // TODO(fengliuai): make storage_type_width and narrow_range configurable.
  Type final_type;
  const auto it = optimized_weights_.find(op);
  const bool is_weight = it != optimized_weights_.end();
  const bool is_weight_with_per_channel_support =
      is_weight && it->second != -1 && is_signed_;

  if (is_weight_with_per_channel_support && !disable_per_channel_) {
    // When `disable_per_channel_` is false, per-channel symmetric quantization
    // parameters are created from the weights when the ops support per-channel
    // quantization. Otherwise, uses per-tensor asymmetric quantization with
    // narrow range.

    // per-axis quantization weight, with symmetric min/max enforced.
    final_type = GetUniformQuantizedPerAxisTypeForWeight(
        attr, it->second, /*symmetric=*/true, /*num_bits=*/8, is_signed_,
        /*narrow_range=*/true, legacy_float_scale_);
  } else {
    // per-tensor quantization weight
    final_type = GetUniformQuantizedTypeForWeight(
        attr, /*symmetric=*/is_weight && is_signed_,
        /*num_bits=*/8, is_signed_,
        /*narrow_range_=*/is_weight, legacy_float_scale_);
  }
  if (const auto quant_type =
          final_type.dyn_cast_or_null<quant::QuantizedType>()) {
    return SetResultParams(op, 0, quant_type);
  }
  return false;
}

bool QuantizationDriver::SetResultParams(Operation* op, const int res_index,
                                         const QuantParams params) {
  auto& state = GetResultQuantState(op, res_index);
  if (state.params == params) {
    return false;
  }
  if (!state.IsEmpty()) {
    auto& rescales = GetResultRequantizeStates(op, res_index);
    RequantizeState& rescale = rescales.emplace_back();
    rescale.pos = RequantizeState::ON_INPUT;
    rescale.params = params;
    return true;
  }
  state.params = params;
  AddUserToList(op, res_index);
  return true;
}

QuantParams QuantizationDriver::GetBiasParams(
    Operation* op, const int bias_index, const std::vector<int>& non_biases,
    const AccumulatorScaleFunc func) {
  QuantState& bias_state = GetOperandQuantState(op, bias_index);
  if (!bias_state.IsEmpty()) {
    return bias_state.params;
  }
  std::vector<QuantParams> op_types;
  op_types.reserve(non_biases.size());
  int adjusted_quant_dim = -1;
  if (op->getNumOperands() > bias_index) {
    // Some kernels allow 1D bias, broadcasting it inside the kernel. In this
    // case, the `quantizedDimension=0` when quantizing per-channel.
    // However, for some kernels which require bias to be already broadcasted
    // to match the accumulation shape, the very last index should be used.
    Operation* bias_op = op->getOperand(bias_index).getDefiningOp();
    if (bias_op != nullptr) {
      Type bias_type = bias_op->getResult(0).getType();
      if (bias_type != builder_.getNoneType()) {
        const int bias_rank = bias_type.dyn_cast<ShapedType>().getRank();
        adjusted_quant_dim = bias_rank > 1 ? bias_rank - 1 : 0;
      }
    }
  }

  for (int non_bias : non_biases) {
    const QuantState& non_bias_type = GetOperandQuantState(op, non_bias);
    op_types.push_back(non_bias_type.params);
  }
  return func(op_types, adjusted_quant_dim, legacy_float_scale_);
}

bool QuantizationDriver::SetOperandParams(Operation* op, const int index,
                                          const QuantParams params,
                                          const bool override) {
  auto& state = GetOperandQuantState(op, index);
  if (state.params == params) {
    return false;
  }

  if (!state.IsEmpty() && !override) {
    auto& rescales = GetOperandRequantizeStates(op, index);
    for (RequantizeState& rescale : rescales) {
      if (rescale.params == params) {
        rescale.users.emplace_back(op, index);
        return true;
      }
    }
    RequantizeState& rescale = rescales.emplace_back();
    rescale.pos = RequantizeState::ON_OUTPUT;
    rescale.params = params;
    rescale.users.emplace_back(op, index);
    return true;
  }

  state.params = params;
  AddOperandToList(op, index);
  return true;
}

void QuantizationDriver::QuantizeOpResult(Operation* op, const int index,
                                          const QuantParams params) {
  builder_.setInsertionPointAfter(op);
  const Value original_result = op->getResult(index);
  QuantizeValue(original_result, params, op->getLoc());
}

void QuantizationDriver::QuantizeArg(BlockArgument arg, QuantParams params) {
  builder_.setInsertionPointToStart(arg.getOwner());
  QuantizeValue(arg, params, builder_.getUnknownLoc());
}

void QuantizationDriver::QuantizeValue(Value value, QuantParams params,
                                       Location loc) {
  const Type expressed_type = value.getType();
  const Type new_type = params.castFromExpressedType(expressed_type);
  // This value isn't an expressed type (float), skip.
  if (!new_type) return;
  auto quantize =
      builder_.create<quantfork::QuantizeCastOp>(loc, new_type, value);
  auto dequantize = builder_.create<quantfork::DequantizeCastOp>(
      loc, expressed_type, quantize.getResult());

  // This attribute is set to distinguish the quantize ops being added by the
  // quantization pass. These ops can be removed without losing original
  // program accuracy.
  // TODO(fengliuai): make the attribute being part of op definition.
  quantize->setAttr(kVolatileOpAttrName, builder_.getUnitAttr());

  // `original_result` has a use to `quantize`, so this will replace that use
  // by the result of `dequantize`. Remember to reset that use afterwards
  value.replaceAllUsesWith(dequantize);
  quantize.getOperation()->replaceUsesOfWith(dequantize, value);
}

void QuantizationDriver::RequantizeOpResult(Operation* op, const int index,
                                            RequantizeStates* states) {
  if (states->empty()) return;

  builder_.setInsertionPointAfter(op);
  Value value = op->getResult(index);
  RequantizeState::RequantizePosition pos = states->front().pos;
  if (pos == RequantizeState::NO_REQUANTIZE) {
    return;
  }
  for (auto& state : *states) {
    // Check that all requantization positions are the same for each state.
    // Unsure if this check is required.
    if (state.pos != pos) {
      return;
    }
  }
  if (pos == RequantizeState::ON_OUTPUT) {
    Operation* user = value.getUses().begin().getUser();
    if (llvm::isa<quantfork::QuantizeCastOp>(user)) {
      // The requantize op is inserted between `quantize` and `dequantize` ops.
      value = user->getResult(0);
      builder_.setInsertionPointAfter(user);
    }
  }
  RequantizeValue(value, states, op->getLoc());
}

void QuantizationDriver::RequantizeArg(const BlockArgument arg,
                                       RequantizeStates* states) {
  Value value = arg;
  builder_.setInsertionPointToStart(arg.getOwner());
  if (value.hasOneUse()) {
    Operation* user = value.use_begin().getUser();
    if (auto q = llvm::dyn_cast<quantfork::QuantizeCastOp>(user)) {
      value = q.getResult();
      builder_.setInsertionPoint(arg.getOwner(), ++Block::iterator(user));
    }
  }
  RequantizeValue(value, states, builder_.getUnknownLoc());
}

void QuantizationDriver::RequantizeValue(Value value, RequantizeStates* states,
                                         const Location loc) {
  if (states->empty() ||
      states->front().pos == RequantizeState::NO_REQUANTIZE) {
    return;
  }
  if (states->front().pos == RequantizeState::ON_INPUT) {
    auto& state = states->front();
    const Type expressed_type = value.getType();
    // The value needs to be requantized. A Quantize op will be created to use
    // it as the operand and replace its uses.
    const Type new_type = state.params.castFromExpressedType(expressed_type);
    if (!new_type) return;
    auto requantize_op =
        builder_.create<quantfork::QuantizeCastOp>(loc, new_type, value);
    value.replaceAllUsesWith(requantize_op);
    requantize_op.getOperation()->replaceUsesOfWith(requantize_op, value);
    // This requantization was defined as required for the result value, so
    // there should be only one requant state.
    return;
  }

  // If this is an operand that requires requantization, then the value should
  // only have one `DequantizeCastOp` user which produces the operand value.
  if (!value.hasOneUse()) {
    return;
  }
  auto dequant_op = llvm::dyn_cast_or_null<quantfork::DequantizeCastOp>(
      value.use_begin().getUser());
  if (!dequant_op) {
    return;
  }
  // It is possible that the dequant value is used by a op that doesn't require
  // requant, so only overwrite the first if that is not the case.
  const int num_uses = std::distance(dequant_op.getResult().use_begin(),
                                     dequant_op.getResult().use_end());

  // Whether to replace quantization params of the first dequantize op
  // after the quantized value is produced.
  // If there is a use other than the requantize states, then we can't clobber.
  bool clobber_first = num_uses <= states->size();
  for (auto& state : *states) {
    Type expressed_type =
        quant::QuantizedType::castToExpressedType(value.getType());
    if (!expressed_type) continue;
    // The value needs to be requantized. A Quantize op will be created to use
    // it as the operand and replace its uses.
    const Type new_type = state.params.castFromExpressedType(expressed_type);
    // This value isn't an expressed type (float), skip.
    if (!new_type) continue;

    auto requantize_op =
        builder_.create<quantfork::QuantizeCastOp>(loc, new_type, value);

    if (clobber_first) {
      dequant_op.setOperand(requantize_op.getResult());
      // All ops requiring this value already use the result of dequant.
      clobber_first = false;
    } else {
      auto new_dequant_op = builder_.create<quantfork::DequantizeCastOp>(
          loc, dequant_op.getResult().getType(), requantize_op.getResult());
      for (auto& op_index : state.users) {
        op_index.first->setOperand(op_index.second, new_dequant_op.getResult());
      }
    }
  }
}

// A heuristic to get quantization parameters satisfies the same scale
// constraints:
// - If there are immutable states,
//   - use the single input, or,
//   - use the single output, or,
//   - use the first one in the collection,
// - use the single input if it is ready, or,
// - use the single output if it is ready, or,
// - use the first ready one in the collection.
QuantParams QuantizationDriver::GetQuantParamsForSameScaleConstraint(
    Operation* op) {
  // Two vector to collect Non-empty operands and results states.
  std::vector<QuantState*> mutable_states, immutable_states;
  for (int i = 0; i < op->getNumOperands(); ++i) {
    auto& state = GetOperandQuantState(op, i);
    if (state.immutable) {
      immutable_states.push_back(&state);
    } else if (!state.IsEmpty()) {
      mutable_states.push_back(&state);
    }
  }

  const int immutable_operands_num = immutable_states.size();
  const int mutable_operands_num = mutable_states.size();
  // Use the operand's state if it is immutable and it is the only one
  // operand.
  if (op->getNumOperands() == 1 && immutable_operands_num == 1) {
    return immutable_states.front()->params;
  }

  for (int i = 0; i < op->getNumResults(); ++i) {
    auto& state = GetResultQuantState(op, i);
    if (state.immutable) {
      immutable_states.push_back(&state);
    } else if (!state.IsEmpty()) {
      mutable_states.push_back(&state);
    }
  }

  const int immutable_results_num =
      immutable_states.size() - immutable_operands_num;
  const int mutable_results_num = mutable_states.size() - mutable_operands_num;
  // Use the result's state if it is immutable and it is the only one result.
  if (op->getNumResults() == 1 && immutable_results_num == 1) {
    return immutable_states.back()->params;
  }

  // Use the first immutable state to quantize the rest operands and results.
  if (!immutable_states.empty()) return immutable_states.front()->params;

  // If there are no immutable states, use the operand's state if it is the
  // only one operand and has parameters propagated.
  if (op->getNumOperands() == 1 && mutable_operands_num == 1) {
    return mutable_states.front()->params;
  }

  // If there are no immutable states, use the result's state if it is the
  // only one result and has parameters propagated.
  if (op->getNumResults() == 1 && mutable_results_num == 1) {
    return mutable_states.back()->params;
  }

  // Use the first propagated state to quantize the rest operands and results.
  if (!mutable_states.empty()) return mutable_states.front()->params;

  // None operands/results have parameters propagated, skip this node for now.
  return {};
}

void QuantizationDriver::PreprocessConstantOps() {
  fn_.walk([&](arith::ConstantOp cst) {
    // Non-float tensors are neither weights nor require quantization.
    const auto type = cst.getType().dyn_cast<ShapedType>();
    if (!type || !type.getElementType().isa<FloatType>()) return;

    // Skip if the value is NaN or INF.
    // Otherwise the illegal scale/zp will be calculated.
    auto float_attr = cst.getValueAttr().dyn_cast<DenseFPElementsAttr>();
    if (float_attr && !float_attr.getValues<APFloat>()[0].isFinite()) return;

    const Value value = cst.getResult();
    builder_.setInsertionPoint(cst);

    // The following loop will change the value uses, thus we cache all the uses
    // needs to be changed.
    llvm::SmallVector<std::pair<Operation*, int>> uses;
    for (auto& use : value.getUses()) {
      uses.push_back({use.getOwner(), use.getOperandNumber()});
    }
    for (const auto& indexed_use : llvm::enumerate(uses)) {
      Operation* user = indexed_use.value().first;
      const int operand_num = indexed_use.value().second;

      const std::unique_ptr<OpQuantSpec> spec = GetQuantSpec(user);
      const std::unique_ptr<OpQuantScaleSpec> scale_spec =
          GetQuantScaleSpec(user);
      const BiasParamsMap biases = spec->biases_params;

      // The quantization parameters of a `weight` shouldn't be determined by
      // other values. So any constants which are not bias, an operand of an
      // op with same scale requirements, and haven't been quantized are
      // weights.
      if (biases.find(operand_num) == biases.end() &&
          !scale_spec->has_same_scale_requirement &&
          !llvm::dyn_cast<quantfork::QuantizeCastOp>(user)) {
        // Needs to scan the content of weights to get the quantization
        // parameters if there are no quantization parameters (FakeQuant ops).
        // For this case, the weight will not be duplicated.
        weights_.insert(cst);
        if (spec->coeff_op_quant_dim.find(operand_num) !=
            spec->coeff_op_quant_dim.end()) {
          optimized_weights_.insert(
              {cst, spec->coeff_op_quant_dim[operand_num]});
        }
      } else {
        // This is a bias or an operand of an op with same scale requirements,
        // so the quantization parameter are propagated from or determined by
        // other values. Duplicate this constant in case it is shared by
        // different users.
        if (uses.size() > 1) {
          auto new_cst =
              builder_.create<arith::ConstantOp>(cst.getLoc(), cst.getValue());
          user->setOperand(operand_num, new_cst);
        }
      }
    }
  });
}

void QuantizationDriver::SetupAllStates() {
  for (auto arg : fn_.getArguments()) {
    args_.push_back(arg);
    Value value = arg;
    // If the argument is quantized, it should only has one user.
    if (arg.hasOneUse()) {
      Operation* user = value.use_begin().getUser();
      if (auto q = llvm::dyn_cast<quantfork::QuantizeCastOp>(user)) {
        value = q.getResult();
      }
    }
    InitializeArgState(arg, value);
  }

  fn_.walk([&](Operation* op) {
    std::unique_ptr<OpQuantScaleSpec> scale_spec = GetQuantScaleSpec(op);
    if (!IsOpQuantizable(op) && !scale_spec->has_same_scale_requirement) {
      return;
    }
    work_list_.push_back(op);

    for (int i = 0; i < op->getNumOperands(); ++i) {
      Value operand = op->getOperand(i);
      if (auto* inst = operand.getDefiningOp()) {
        // If the operand comes from a `quantfork::DequantizeCastOp`, we use
        // the quantized input of this `quantfork::DequantizeCastOp` to set the
        // state.
        if (auto dq = llvm::dyn_cast<quantfork::DequantizeCastOp>(inst)) {
          operand = dq.getArg();
        }
      }
      InitializeOperandState(op, i, operand);
    }

    for (int res = 0; res < op->getNumResults(); ++res) {
      Value result = op->getResult(res);
      // If the result has been quantized, it should only be used by a
      // `quantfork::QuantizeCastOp`. For this case, we uses the quantized
      // result to create the state and mark it immutable.
      if (result.hasOneUse()) {
        Operation* user = result.use_begin().getUser();
        if (auto q = llvm::dyn_cast<quantfork::QuantizeCastOp>(user)) {
          result = q.getResult();
        }
      }
      InitializeResultState(op, res, result);
    }
  });
}

arith::ConstantOp QuantizationDriver::DuplicateConstantOpIfNeeded(
    arith::ConstantOp op, Operation* target_op, const int operand_index) {
  if (op.getResult().hasOneUse()) {
    return op;
  }
  OpBuilder builder(op->getContext());
  builder.setInsertionPointAfter(op);
  arith::ConstantOp new_op = llvm::cast<arith::ConstantOp>(builder.clone(*op));
  target_op->getOpOperand(operand_index).set(new_op.getResult());
  InitializeOperandState(target_op, operand_index, new_op.getResult());
  InitializeResultState(new_op, 0, new_op.getResult());
  return new_op;
}

bool QuantizationDriver::ShouldCheckBiasScale(
    Operation* op, const int bias_index, const std::vector<int>& input_indices,
    const QuantParams params, int& input_index, int& filter_index) {
  // For now, restrict scale adjustment to ops with affine quantized weights,
  // and having weights and biases as constants. This currently only applies to
  // FC and Conv* ops. Restriction for the weight can be relaxed if there are
  // needs for adjusting scale of variable weights.
  auto affine_op = llvm::dyn_cast<AffineQuantizedOpInterface>(op);
  auto bias_op = op->getOperand(bias_index).getDefiningOp<arith::ConstantOp>();
  if (!affine_op || !bias_op || input_indices.size() != 2) return false;
  if (!bias_op.getValue().isa<DenseFPElementsAttr>()) return false;
  filter_index = affine_op.GetAffineOperandIndex();
  if (!op->getOperand(filter_index).getDefiningOp<arith::ConstantOp>()) {
    return false;
  }
  if (filter_index == input_indices[0]) {
    input_index = input_indices[1];
  } else if (filter_index == input_indices[1]) {
    input_index = input_indices[0];
  } else {
    return false;
  }

  const auto input_state = GetOperandQuantState(op, input_index);
  const auto filter_state = GetOperandQuantState(op, filter_index);
  // If quantization parameter for the filter is fixed, should return it as-is.
  // Only checks ops with 8-bit input and weights, and 32-bit biases.
  if (!(input_state.params.getStorageTypeIntegralWidth() == 8 &&
        filter_state.params.getStorageTypeIntegralWidth() == 8 &&
        params.getStorageTypeIntegralWidth() == 32)) {
    return false;
  }
  return true;
}

bool QuantizationDriver::SetBiasParamsWithAdjustments(
    Operation* op, const int bias_index, const std::vector<int>& input_indices,
    const QuantParams params) {
  bool changed = false;
  int input_index;
  int filter_index;
  if (!ShouldCheckBiasScale(op, bias_index, input_indices, params, input_index,
                            filter_index)) {
    return SetOperandParams(op, bias_index, params);
  }

  quant::QuantState input_state = GetOperandQuantState(op, input_index);
  quant::QuantState filter_state = GetOperandQuantState(op, filter_index);
  auto bias_op = op->getOperand(bias_index).getDefiningOp<arith::ConstantOp>();
  const double input_scale =
      input_state.params.cast<UniformQuantizedType>().getScale();

  auto bias_values = bias_op.getValue().cast<DenseFPElementsAttr>();
  // Restrict maximum absolute value of bias within INT_MAX / 2, to make some
  // room for accumulator.
  const int32_t kBiasMax = std::numeric_limits<int32_t>::max() / 2;
  if (auto bias_params = params.dyn_cast<UniformQuantizedType>()) {
    double bias_half_range = 0.0f;
    for (auto bias : bias_values.getValues<APFloat>()) {
      if (bias_half_range < std::abs(bias.convertToFloat())) {
        bias_half_range = std::abs(bias.convertToFloat());
      }
    }
    if (bias_half_range / bias_params.getScale() < kBiasMax) {
      return SetOperandParams(op, bias_index, params);
    }
    const double new_bias_scale =
        static_cast<double>(bias_half_range) / kBiasMax;

    changed |= SetOperandParams(
        op, bias_index,
        UniformQuantizedType::getChecked(
            bias_op->getLoc(), params.getFlags(), params.getStorageType(),
            params.getExpressedType(), new_bias_scale, 0,
            params.getStorageTypeMin(), params.getStorageTypeMax()));
    auto filter_op = DuplicateConstantOpIfNeeded(
        op->getOperand(filter_index).getDefiningOp<arith::ConstantOp>(), op,
        filter_index);
    if (!filter_op) {
      return SetOperandParams(op, bias_index, params);
    }

    const auto filter_param = filter_state.params.cast<UniformQuantizedType>();
    changed |= SetOperandParams(
        op, filter_index,
        UniformQuantizedType::getChecked(
            filter_op->getLoc(), filter_param.getFlags(),
            filter_param.getStorageType(), filter_param.getExpressedType(),
            new_bias_scale / input_scale, 0, filter_param.getStorageTypeMin(),
            filter_param.getStorageTypeMax()),
        /*override=*/true);
  } else if (auto bias_params =
                 params.dyn_cast<quant::UniformQuantizedPerAxisType>()) {
    const auto filter_params =
        filter_state.params.cast<quant::UniformQuantizedPerAxisType>();
    std::vector<double> new_bias_scales = bias_params.getScales().vec();
    std::vector<double> new_filter_scales = filter_params.getScales().vec();
    bool needs_adjustment = false;
    for (int i = 0; i < bias_params.getScales().size(); ++i) {
      const float abs_bias = std::abs(bias_values.getValues<float>()[i]);
      if (abs_bias / new_bias_scales[i] > kBiasMax) {
        new_bias_scales[i] = static_cast<double>(abs_bias) / kBiasMax;
        new_filter_scales[i] = new_bias_scales[i] / input_scale;
        needs_adjustment = true;
      }
    }
    if (!needs_adjustment) {
      return SetOperandParams(op, bias_index, params);
    }
    changed |= SetOperandParams(
        op, bias_index,
        quant::UniformQuantizedPerAxisType::getChecked(
            bias_op->getLoc(), params.getFlags(), params.getStorageType(),
            params.getExpressedType(), new_bias_scales,
            bias_params.getZeroPoints(), bias_params.getQuantizedDimension(),
            params.getStorageTypeMin(), params.getStorageTypeMax()));

    auto filter_op = DuplicateConstantOpIfNeeded(
        op->getOperand(filter_index).getDefiningOp<arith::ConstantOp>(), op,
        filter_index);
    changed |= SetOperandParams(
        op, filter_index,
        quant::UniformQuantizedPerAxisType::getChecked(
            filter_op->getLoc(), filter_params.getFlags(),
            filter_params.getStorageType(), filter_params.getExpressedType(),
            new_filter_scales, filter_params.getZeroPoints(),
            filter_params.getQuantizedDimension(),
            filter_params.getStorageTypeMin(),
            filter_params.getStorageTypeMax()),
        /*override=*/true);
  }
  return changed;
}

// This method scans the operations in the function to setup the initial
// states for quantization parameter propagation.
// TODO(fengliuai): This algorithm assumes there are only one pair of
// `quantfork::QuantizeCastOp` and `quantfork::DequantizeCastOp` ops between two
// quantizable ops. A sanity check should be applied.
void QuantizationDriver::Initialize() {
  // Duplicate the bias constant, so the states can be setup correctly.
  // TODO(fengliuai): Function definition should also be duplicated if there
  // are multiple call sites.
  PreprocessConstantOps();

  // Setup all the internal states.
  SetupAllStates();
}

// Propagates the quantization parameters to the operands, results, and biases.
// TODO: b/323478683 - Do not use while loop to handle this logic.
bool QuantizationDriver::PropagateParamsAndReturnIfChanged() {
  // TODO(fengliuai): uses a typed indicator instead of a bool value.
  bool changed = false;
  while (!work_list_.empty()) {
    Operation* op = work_list_.back();
    work_list_.pop_back();

    // This op has been quantized, so we should not consider it again.
    if (llvm::is_contained(quantized_, op)) continue;
    quantized_.insert(op);

    if (auto cst = llvm::dyn_cast<arith::ConstantOp>(op)) {
      // If the workflow requires inferring ranges from the content
      // (post-training quantization) and it is weight (filter) and hasn't
      // been quantized, we infer the quantization parameters from the content.
      if (infer_tensor_range_ && IsWeight(cst) && !IsQuantized(op)) {
        // The quantization parameters are determined by the content of the
        // constant.
        changed |= SetConstantResultParams(op);
      }
      continue;
    }

    std::unique_ptr<OpQuantScaleSpec> scale_spec = GetQuantScaleSpec(op);

    if (scale_spec->has_same_scale_requirement) {
      const auto params = GetQuantParamsForSameScaleConstraint(op);
      // The quantization parameters haven't been propagated to any operands
      // or results. Skip this node for now.
      if (!params) {
        quantized_.erase(op);
        continue;
      }

      // Use the final state to set all the operands' parameters.
      for (int i = 0; i < op->getNumOperands(); ++i) {
        if (auto type = op->getOperand(i).getType().dyn_cast<ShapedType>()) {
          // Without this check, it will accidentally propagate the quantization
          // information by the shared non-float tensors.
          if (type.getElementType().isa<FloatType>())
            changed |= SetOperandParams(op, i, params);
        }
      }

      // Use the final state to set all the results' parameters.
      for (int res = 0; res < op->getNumResults(); ++res)
        if (auto type = op->getResult(res).getType().dyn_cast<ShapedType>()) {
          // Without this check, it will accidentally propagate the quantization
          // information by the shared non-float-tensors.
          if (type.getElementType().isa<FloatType>())
            changed |= SetResultParams(op, res, params);
        }
    }

    // If the model already contains immutable QDQs, require upstream to
    // explicitly fix output range instead.
    if (scale_spec->has_fixed_output_range && infer_tensor_range_ &&
        !is_qdq_conversion_) {
      // Infer ranges from the activation ops. This is usually required for
      // the post-training quantization workflow.
      // TODO(fengliuai): different result can have different fixed range.
      const auto params =
          scale_spec->fixed_output_range_func(is_signed_, bit_width_);
      for (auto i = 0; i < op->getNumResults(); ++i) {
        // The range is null if the result has been quantized.
        if (params) {
          changed |= SetResultParams(op, i, params);
        }
      }
    }

    const auto spec = GetQuantSpec(op);
    for (auto& it : spec->biases_params) {
      const auto params =
          GetBiasParams(op, it.first, it.second.first, it.second.second);
      if (!params) {
        quantized_.erase(op);
        continue;
      }
      changed |=
          SetBiasParamsWithAdjustments(op, it.first, it.second.first, params);
    }
  }

  return changed;
}

// Finalizes the arguments and result states in the function.
void QuantizationDriver::Finalize() {
  for (auto arg : args_) {
    auto& state = GetArgQuantState(arg);
    auto& requantizes = GetArgRequantizeStates(arg);
    if (state.IsEmpty() || (state.immutable && requantizes.empty())) {
      continue;
    }

    if (!state.immutable) {
      QuantizeArg(arg, state.params);
    }

    if (!requantizes.empty()) {
      RequantizeArg(arg, &requantizes);
    }
  }

  for (auto it : result_states_) {
    Operation* op = it.first.first;
    const int res_index = it.first.second;
    auto& state = GetResultQuantState(op, res_index);
    auto& requantizes = GetResultRequantizeStates(op, res_index);
    if (state.IsEmpty() || (state.immutable && requantizes.empty())) {
      continue;
    }

    if (!state.immutable) {
      QuantizeOpResult(op, res_index, state.params);
    }

    if (!requantizes.empty()) {
      RequantizeOpResult(op, res_index, &requantizes);
    }
  }
}

// Runs quantization in following steps:
//   1. Scans the operations in the function to setup the initial
//      states for quantization parameter propagation.
//   2. Propagates the quantization parameters to the operands, results, and
//      biases.
//   3. Finalizes the arguments and result states in the function.
void QuantizationDriver::Run() {
  Initialize();
  if (PropagateParamsAndReturnIfChanged()) {
    Finalize();
  }
}

void ApplyQuantizationParamsPropagation(
    const mlir::func::FuncOp func, const bool is_signed, const int bit_width,
    const bool disable_per_channel,
    const OpQuantSpecGetter op_quant_spec_getter,
    const bool infer_tensor_ranges, const bool legacy_float_scale,
    const bool is_qdq_conversion) {
  ApplyQuantizationParamsPropagation(
      func, is_signed, bit_width, disable_per_channel, op_quant_spec_getter,
      GetDefaultQuantScaleSpec, infer_tensor_ranges, legacy_float_scale,
      is_qdq_conversion);
}

void ApplyQuantizationParamsPropagation(
    const mlir::func::FuncOp func, const bool is_signed, const int bit_width,
    const bool disable_per_channel,
    const OpQuantSpecGetter op_quant_spec_getter,
    const OpQuantScaleSpecGetter op_quant_scale_spec_getter,
    const bool infer_tensor_ranges, const bool legacy_float_scale,
    const bool is_qdq_conversion) {
  QuantizationDriver(func, is_signed, bit_width, disable_per_channel,
                     op_quant_spec_getter, op_quant_scale_spec_getter,
                     infer_tensor_ranges, legacy_float_scale, is_qdq_conversion)
      .Run();
}

}  // namespace quant
}  // namespace mlir
