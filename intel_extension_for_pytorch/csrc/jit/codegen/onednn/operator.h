#pragma once

#include <oneapi/dnnl/dnnl_graph.hpp>
#include <torch/csrc/jit/ir/ir.h>
#include "csrc/jit/codegen/LlgaTensorImpl.h"

namespace torch {
namespace jit {
namespace fuser {
namespace onednn {

class Operator {
 public:
  Operator(const Node* node, dnnl::graph::op::kind kind)
      : n(node), o(getId(node), kind, node->kind().toQualString()), k(kind) {}

  Operator& setInputValue(Value* v) {
    if (v->mustNotBeNone())
      o.add_input(createLogicalTensor(v));
    return *this;
  }

  Operator& setInput(size_t offset) {
    return setInputValue(n->input(offset));
  }

  template <typename... Ts>
  Operator& setInput(size_t offset, Ts... other) {
    setInput(offset);
    return setInput(other...);
  }

  Operator& setOutputValue(Value* v) {
    if (v->mustNotBeNone())
      o.add_output(createLogicalTensor(v));
    return *this;
  }

  Operator& setOutput(size_t offset) {
    return setOutputValue(n->output(offset));
  }

  template <typename... Ts>
  Operator& setOutput(size_t offset, Ts... other) {
    setOutput(offset);
    return setOutput(other...);
  }

  template <typename Attr>
  Operator& setAttr(std::string name, Attr&& attr) {
    o.set_attr(name, std::forward<Attr>(attr));
    return *this;
  }

  template <typename F>
  Operator& setAttr(std::string name, const F& fn, size_t offset) {
    return setAttr(name, fn(n, offset));
  }

  static std::vector<int64_t> Ints(const Node* node, size_t offset) {
    return toIValue(node->input(offset))->toIntVector();
  }

  static int64_t Int(const Node* node, size_t offset) {
    return toIValue(node->input(offset))->toInt();
  }

  static float Float(const Node* node, size_t offset) {
    return static_cast<float>(toIValue(node->input(offset))->toDouble());
  }

  static std::vector<float> FloatValueToVector(float value) {
    return {value};
  }

  static std::vector<float> FloatToVector(const Node* node, size_t offset) {
    return FloatValueToVector(Float(node, offset));
  }

  static std::vector<int64_t> IntValueToVector(int64_t value) {
    return {value};
  }

  static std::vector<int64_t> IntToVector(const Node* node, size_t offset) {
    return IntValueToVector(Int(node, offset));
  }

  static std::string QuantString(at::ScalarType scalar_type) {
    switch (scalar_type) {
      case at::ScalarType::QInt8:
        return std::string("int8");
      case at::ScalarType::QUInt8:
        return std::string("uint8");
      default:
        TORCH_CHECK(
            false,
            "Invalid quant data type ",
            static_cast<size_t>(scalar_type));
    }
  }

  static std::string String(const Node* node, size_t offset) {
    return QuantString(static_cast<at::ScalarType>(Int(node, offset)));
  }

  static at::Tensor Tensor(const Node* node, size_t offset) {
    return toIValue(node->input(offset))->toTensor();
  }

  static bool Bool(const Node* node, size_t offset) {
    return toIValue(node->input(offset))->toBool();
  }

  static uint64_t getId(const Node* node) {
    return reinterpret_cast<uint64_t>(node); // cast node address as op id
  }

  static Node* getNode(uint64_t opId) {
    return reinterpret_cast<Node*>(opId);
  }

  dnnl::graph::op::kind kind() const {
    return k;
  }

  dnnl::graph::op llgaOp() const {
    return o;
  }

 private:
  dnnl::graph::logical_tensor createLogicalTensor(Value* value) const {
    return at::LlgaTensorDesc(value).logical_tensor();
  }

  const Node* n;
  dnnl::graph::op o;
  dnnl::graph::op::kind k;
};

} // namespace onednn
} // namespace fuser
} // namespace jit
} // namespace torch