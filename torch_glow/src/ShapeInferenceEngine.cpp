// Copyright 2004-present Facebook. All Rights Reserved.

#include <iostream>
#include <string>
#include <torch/script.h>
#include <unordered_set>
#include <vector>

#include "ShapeInferenceEngine.h"

#include "glow/Support/Error.h"
#include "glow/Support/Support.h"

namespace glow {

ShapeInferenceEngine::ShapeInferenceEngine(
    const torch::jit::Graph &graph, const at::ArrayRef<at::IValue> &inputs)
    : graph_(graph), inputs_(inputs){};

void ShapeInferenceEngine::getNodeInputShape(const torch::jit::Node *node,
                                             MetaStack &inputMetas) {
  for (auto input : node->inputs()) {
    auto it = shapeMap_.find(input);
    CHECK(it != shapeMap_.end());
    inputMetas.emplace_back(shapeMap_[input]);
  }
}

std::vector<std::vector<int64_t>> &ShapeInferenceEngine::getGraphOutputShape() {
  return outputShape_;
}

Error ShapeInferenceEngine::shapeOnNode(const torch::jit::Node *node) {

  /// Get op symbol
  const auto kind = node->kind();

  /// Extract shapes of inputs from shape mapping
  MetaStack inputMetas;

  /// The output of each Op shape function could be either the shape or int
  /// value generated by prim::consant or prim::ListContruct.
  /// The \p outputShapesOrValues is to store outputs of ops shape function.
  std::vector<std::vector<int64_t>> outputShapesOrValues(1);

  getNodeInputShape(node, inputMetas);

  // Get output shape or int value of the ops without actual computation
  switch (kind) {
  case c10::prim::Constant: {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], primConstant(node));
    break;
  }
  case c10::aten::tanh:
  case c10::aten::relu:
  case c10::aten::sigmoid: {
    RETURN_ERR_IF_NOT(inputMetas.size() == 1,
                      "Expected 1 input shape for operators.");
    outputShapesOrValues[0] = inputMetas[0].shape;
    break;
  }
  case c10::aten::sub:
  case c10::aten::pow:
  case c10::aten::mul:
  case c10::aten::add: {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], binaryOp(inputMetas));
    break;
  }
  case c10::aten::mm: {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], mm(inputMetas));
    break;
  }
  case c10::aten::addmm: {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], addmm(inputMetas));
    break;
  }
  case c10::aten::bmm: {
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0], bmm(inputMetas));
    break;
  }
  case c10::prim::FusedConcat: {
    int64_t dim = node->i(at::attr::dim);
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues[0],
                               fusedConcat(inputMetas, dim));
    break;
  }
  case c10::prim::ConstantChunk: {
    int64_t chunks = node->i(at::attr::chunks);
    int64_t dim = node->i(at::attr::dim);
    ASSIGN_VALUE_OR_RETURN_ERR(outputShapesOrValues,
                               constantChunk(inputMetas, chunks, dim));
    break;
  }
  default: {
    return MAKE_ERR(
        strFormat("Node's operator %s is not supported", kind.toQualString()));
  }
  }

  /// Put output into map
  /// For \p prim::Constant, the output could be either Tensor or NumberType.
  /// If the output is TensorType, store the \p outputShapesOrValues
  /// into VariableMeta.shape;
  /// Else store the \p outputShapesOrValues into VariableMeta.intValue.
  if (kind == c10::prim::Constant) {
    if (node->output()->type()->isSubtypeOf(at::TensorType::get())) {
      shapeMap_[node->output()].shape = std::move(outputShapesOrValues[0]);
    } else {
      shapeMap_[node->output()].shape = {1};
      shapeMap_[node->output()].intValue = std::move(outputShapesOrValues[0]);
    }
  } else {
    for (int i = 0; i < node->outputs().size(); i++) {
      shapeMap_[node->output(i)].shape = std::move(outputShapesOrValues[i]);
    }
  }
  return Error::success();
}

Error ShapeInferenceEngine::run() {

  RETURN_ERR_IF_NOT(
      inputs_.size() == graph_.inputs().size(),
      "Number of inputs mismatch between Graph and actual inputs");

  /// Put graph input into shape mapping
  RETURN_IF_ERR(getGraphIntputShape());

  /// Run shape inference for each node
  for (auto *node : graph_.nodes()) {
    RETURN_IF_ERR(shapeOnNode(node));
  }

  /// Extract output from shape mapping
  generateGraphOutputShape();
  return Error::success();
}

void ShapeInferenceEngine::printShapeMap() {
  for (auto elem : shapeMap_) {
    std::cout << elem.first->debugName() << ":[ ";
    for (auto value : elem.second.shape) {
      std::cout << value << " ";
    }
    std::cout << "]" << std::endl;
  }
}

/// If the input is tensor, store the shape info only;
/// Else If the input is bool or int, store the value, and set shape as 1.
/// Else if the input is intlist, store the intlist, and set shape as [sizeof
/// intlist, 1]
/// Else return an error
Error ShapeInferenceEngine::getGraphIntputShape() {
  for (auto i = 0; i < inputs_.size(); i++) {
    auto gInName = graph_.inputs()[i];
    shapeMap_[gInName].shape = {};
    shapeMap_[gInName].intValue = {};

    auto input = inputs_[i];
    if (input.isTensor()) {
      auto ptTensor = input.toTensor();
      for (auto s : ptTensor.sizes()) {
        shapeMap_[gInName].shape.emplace_back(s);
      }
    } else if (input.isBool() || input.isInt()) {
      shapeMap_[gInName].shape = {1};
      shapeMap_[gInName].intValue = {input.toInt()};
    } else if (input.isIntList()) {
      auto ptIntList = input.toIntVector();
      shapeMap_[gInName].shape = {static_cast<long>(ptIntList.size()), 1};
      shapeMap_[gInName].intValue = ptIntList;
    } else {
      return MAKE_ERR("Input type doesn't support yet.");
    }
  }
  return Error::success();
}

void ShapeInferenceEngine::generateGraphOutputShape() {
  for (auto output : graph_.outputs()) {
    auto it = shapeMap_.find(output);
    CHECK(it != shapeMap_.end());
    outputShape_.emplace_back(it->second.shape);
  }
}

/// The \p prim::Constant may have multiple types of output, eg.
/// int = prim::Constant[value=0]()
/// Float(1:1) = prim::Constant[value={0}]()
/// bool = prim::Constant[value=0]()
/// None = prim::Constant()
/// Tensor = prim::Constant[value= <Tensor>]()
/// If the output is a tensor, return shape info;
/// Else, return the value.
Expected<std::vector<int64_t>>
ShapeInferenceEngine::primConstant(const torch::jit::Node *node) {

  std::vector<int64_t> shapeOrValue;
  at::TypePtr type = node->output()->type();

  if (type->isSubtypeOf(at::FloatType::get())) {
    /// The float type will not affect the shape
    /// Set value as 1
    shapeOrValue = {1};
  } else if (type->isSubtypeOf(at::IntType::get())) {
    shapeOrValue = {node->i(at::attr::value)};
  } else if (type->isSubtypeOf(at::BoolType::get())) {
    shapeOrValue = {node->i(at::attr::value)};
  } else if (type->isSubtypeOf(at::NoneType::get())) {
    shapeOrValue = {};
  } else if (type->isSubtypeOf(at::TensorType::get())) {
    at::Tensor t = node->t(at::attr::value);
    for (auto s : t.sizes()) {
      shapeOrValue.emplace_back(s);
    }
  }
  return shapeOrValue;
}

/**
 * aten::add(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * aten::pow(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * aten::mul(Tensor self, Tensor or Scalar other, Scalar alpha=1) -> Tensor
 * varibableMetas: 0: self, 1: other
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::binaryOp(const MetaStack &varibableMetas) {

  if (varibableMetas.size() != 2 && varibableMetas.size() != 3) {
    return MAKE_ERR("Expected two or three inputs shapes of this operation.");
  }

  const std::vector<int64_t> &t0 = varibableMetas[0].shape;
  const std::vector<int64_t> &t1 = varibableMetas[1].shape;

  auto d0 = t0.size();
  auto d1 = t1.size();

  /// One input is Scalar
  if (d1 == 1) {
    return t0;
  }

  size_t dim = std::max(d0, d1);
  std::vector<int64_t> shape(dim);

  for (auto i = 0; i < dim; i++) {
    auto j = -1 - i;
    if (i >= d0 || t0[d0 + j] == 1) {
      shape[dim + j] = t1[d1 + j];
    } else if (i >= d1 || t1[d1 + j] == 1) {
      shape[dim + j] = t0[d0 + j];
    } else {
      if (t1[d1 + j] != t0[d0 + j]) {
        return MAKE_ERR(
            strFormat("The size of tensor a (%zu) must match the size of "
                      "tensor b (%zu)at non-singleton dimension 1.",
                      t0[d0 + j], t1[d1 + j]));
      }

      shape[dim + j] = t1[d1 + j];
    }
  }
  return shape;
}

/**
 * aten::mm(Tensor self, Tensor mat2) -> Tensor
 * varibableMetas: 0: self, 1: mat2
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::mm(const MetaStack &varibableMetas) {

  RETURN_ERR_IF_NOT(varibableMetas.size() == 2,
                    "Expected two inputs shapes of this operation.");

  const std::vector<int64_t> &t0 = varibableMetas[0].shape;
  const std::vector<int64_t> &t1 = varibableMetas[1].shape;

  if (!(t1.size() == 2 && t0.size() == 2)) {
    return MAKE_ERR("Expected 2-dimensional tensor.");
  }

  if (t0[1] != t1[0]) {
    return MAKE_ERR(
        strFormat("The size of tensor a (%zu) at dimension 1 must match the "
                  "size of tensor b (%zu) at dimension 0.",
                  t0[1], t1[0]));
  }

  std::vector<int64_t> shape = {t0[0], t1[1]};
  return shape;
}

/**
 * aten::bmm(Tensor self, Tensor mat2) -> Tensor
 * varibableMetas: 0: self, 1: mat2
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::bmm(const MetaStack &varibableMetas) {

  if (varibableMetas.size() != 2) {
    return MAKE_ERR("Expected two inputs shapes of this operation.");
  }

  const std::vector<int64_t> &t0 = varibableMetas[0].shape;
  const std::vector<int64_t> &t1 = varibableMetas[1].shape;

  if (!(t0.size() == 3 && t1.size() == 3)) {
    return MAKE_ERR("Expected 3-dimensional tensor.");
  }

  if (t0[0] != t1[0]) {
    return MAKE_ERR("Expected tensors to have same size at dimension 0");
  }

  if (t0[2] != t1[1]) {
    return MAKE_ERR(strFormat("The size of tensor a (%zu) at dimension 2 must"
                              "match the size of tensor b (%zu) at dimension 1",
                              t0[2], t1[1]));
  }
  std::vector<int64_t> shape = {t0[0], t0[1], t1[2]};
  return shape;
}

/**
 * aten::addmm(Tensor self, Tensor mat1, Tensor mat2, *, Scalar beta=1, Scalar
   alpha=1) -> Tensor
 * varibableMetas: 0: self, 1: mat1, 2: mat2
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::addmm(const MetaStack &varibableMetas) {

  RETURN_ERR_IF_NOT(varibableMetas.size() >= 3,
                    strFormat("Expected at least three inputs shapes, got %zu.",
                              varibableMetas.size()));

  const VariableMeta &t0 = varibableMetas[0];
  const VariableMeta &t1 = varibableMetas[1];
  const VariableMeta &t2 = varibableMetas[2];
  VariableMeta t;

  // For Scalar type, the shape.size() is 1
  if (varibableMetas[2].shape.size() == 1) {
    t = varibableMetas[1];
  } else {
    const MetaStack &mm_shape = {t1, t2};
    ASSIGN_VALUE_OR_RETURN_ERR(t.shape, mm(mm_shape));
  }

  return binaryOp({t0, std::move(t)});
}

/**
 * prim::ConstantChunk[int chunks, int dim](Tensor self) -> Tensors
 * varibableMetas: 0: self
 */
Expected<std::vector<std::vector<int64_t>>>
ShapeInferenceEngine::constantChunk(const MetaStack &varibableMetas,
                                    int64_t chunks, int64_t dim) {

  RETURN_ERR_IF_NOT(
      varibableMetas.size() == 1,
      strFormat("Expected one input, got %zu.", varibableMetas.size()));

  /// Convert dim into positive
  if (dim < 0) {
    dim += varibableMetas[0].shape.size();
  }
  RETURN_ERR_IF_NOT(dim < varibableMetas[0].shape.size() && dim >= 0,
                    "Dim value is out of range.");

  /// For constant chunk, the size of the last chunk one may smaller than the
  /// others
  int64_t c = (varibableMetas[0].shape[dim] + chunks - 1) / chunks;
  int64_t r = varibableMetas[0].shape[dim] - c * (chunks - 1);

  std::vector<std::vector<int64_t>> outShapes;
  for (int i = 0; i < chunks; i++) {
    std::vector<int64_t> shape = varibableMetas[0].shape;
    shape[dim] = (i == chunks - 1) ? r : c;
    outShapes.emplace_back(shape);
  }

  return outShapes;
}

/**
 * prim::FusedConcat[int dim](Tensor self, Tensor mat1, Tensor mat2, ...) ->
 * Tensor varibableMetas: 0: self, 1: mat1, 2: mat2, ...
 */
Expected<std::vector<int64_t>>
ShapeInferenceEngine::fusedConcat(const MetaStack &varibableMetas,
                                  int64_t dim) {

  RETURN_ERR_IF_NOT(
      varibableMetas.size() >= 1,
      strFormat("Expected at least 1 inputs, got %zu.", varibableMetas.size()));

  if (varibableMetas.size() == 1) {
    return varibableMetas[0].shape;
  }

  std::vector<int64_t> shape = varibableMetas[0].shape;
  /// Convert negtive dimension to positive, then check the dim range.
  int64_t inDims = varibableMetas[0].shape.size();
  if (dim < 0) {
    dim += inDims;
  }
  RETURN_ERR_IF_NOT(dim < inDims && dim >= 0, "Dim value is out of range.");

  /// Handle multiple inputs cases
  for (int i = 1; i < varibableMetas.size(); ++i) {
    RETURN_ERR_IF_NOT(inDims == varibableMetas[i].shape.size(),
                      "All inputs must have the same number of dimensions.");
    for (int j = 0; j < inDims; j++) {
      if (j == dim) {
        shape[dim] += varibableMetas[i].shape[dim];
      } else {
        RETURN_ERR_IF_NOT(
            shape[j] == varibableMetas[i].shape[j],
            strFormat("Sizes of tensors must match except in dimension %zu.",
                      dim));
      }
    }
  }
  return shape;
}
} // namespace glow
