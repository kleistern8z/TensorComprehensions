/**
 * Copyright (c) 2017-present, Facebook, Inc.
 *
 * Licensed under the Apache License, Version 2.0 (the "License");
 * you may not use this file except in compliance with the License.
 * You may obtain a copy of the License at
 *
 *     http://www.apache.org/licenses/LICENSE-2.0
 *
 * Unless required by applicable law or agreed to in writing, software
 * distributed under the License is distributed on an "AS IS" BASIS,
 * WITHOUT WARRANTIES OR CONDITIONS OF ANY KIND, either express or implied.
 * See the License for the specific language governing permissions and
 * limitations under the License.
 */
#pragma once

#include <gtest/gtest.h>
#include <mutex>
#include <string>
#include <vector>

#include "tc/aten/aten.h"

#include "caffe2/core/common.h"
#include "caffe2/core/common_gpu.h"
#include "caffe2/core/context_gpu.h"
#include "caffe2/core/net_simple.h"

#include "tc/c2/operator_meta.h"
#include "tc/c2/tc_op.h"
#include "tc/core/cuda/cuda.h"

#include "cuda/test_harness.h"

namespace caffe2 {

// CPUBackend is always used and the source of truth for performing checks
struct CPUBackend {
  static constexpr auto Device = DeviceType::CPU;
  using Context = CPUContext;
  using Tensor = TensorCPU;
};

/// Make a context for the proper Backend type.
/// A DeviceOption may be passed (e.g. to set the random seed).
template <typename Caffe2Backend>
std::unique_ptr<typename Caffe2Backend::Context> makeContext(
    caffe2::DeviceOption opt = DeviceOption());

/// This function retrieves a Caffe2 tensor of the proper backend type
/// from a workspace. The lookup is done by the underlying Blob name in the
/// workspace.
/// The backend type ***must match*** the underlying Blob type because the
/// Blob.Get method is templated and performs correctness checks ar runtime.
/// This function is used for testing purposes, we do not worry about
/// const correctness for now.
template <typename Caffe2Backend>
caffe2::Tensor<typename Caffe2Backend::Context> getNamedTensor(
    caffe2::Workspace& ws,
    const std::string& name);

// helper functions to construct an ATen tensor from a caffe2 tensor
template <typename Caffe2TensorType>
at::Tensor makeATenTensor(
    const Caffe2TensorType& c2Tensor,
    at::Backend backend,
    at::ScalarType stype);

/// We need to provide a way to perform correctness checks on gradients
/// using existing Caffe2 operators.
///
/// The default reference implementation builder can be obtained by calling
/// MakeDefaultReferenceImplementationworks for Caffe2 operators whose
/// gradient reference implementation has been registered properly
/// (in the ReferenceImplementationRegistry). Such operators usually are
/// named TcOpCaffe2OpName (e.g. TcOpMatMul).
///
/// In the case of the generic TcOp, this is not possible because there is
/// no such thing as generic matching of a TC function to a Caffe2 operator
/// (at least not for now). Therefore we need to provide a way to construct
/// a reference implementation for generic TcOp instances.
///
/// This is the purpose of the ReferenceImplementationBuilder.
/// For properly registered TcOp, one can use the default
/// MakeDefaultReferenceImplementationBuilder()
using ReferenceImplementationBuilder =
    std::function<void(const OperatorDef& op_def, NetDef* net_def)>;

ReferenceImplementationBuilder MakeDefaultReferenceImplementationBuilder() {
  return [](const OperatorDef& op_def, NetDef* net_def) {
    caffe2::ReferenceImplementationRegistry::Append(net_def, op_def);
  };
}

namespace {
std::mutex rng_mutex;
}

template <
    class IterableInputs = std::initializer_list<string>,
    class IterableOutputs = std::initializer_list<string>,
    class IterableArgs = std::initializer_list<Argument>>
OperatorDef Configure(
    std::string type,
    IterableInputs ins,
    IterableOutputs outs,
    IterableArgs args = {},
    caffe2::DeviceType dtype = caffe2::CPUBackend::Device) {
  OperatorDef def = CreateOperatorDef(type, "", ins, outs, args);
  def.mutable_device_option()->set_device_type(dtype);
  return def;
}

template <
    class IterableInputs = std::initializer_list<string>,
    class IterableOutputs = std::initializer_list<string>,
    class IterableArgs = std::initializer_list<Argument>>
OperatorDef ConfigureCUDA(
    std::string type,
    IterableInputs ins,
    IterableOutputs outs,
    IterableArgs args = {}) {
  return Configure(type, ins, outs, args, caffe2::CUDABackend::Device);
}

template <typename T>
T* NewTensor(
    caffe2::Workspace& ws,
    std::vector<caffe2::TIndex> shape,
    const std::string& name) {
  caffe2::Blob* blob = ws.CreateBlob(name);
  auto* tensor = blob->GetMutable<T>();
  tensor->Resize(shape);
  return tensor;
}

template <typename Caffe2Backend, typename T>
void AddConstInput(
    caffe2::Workspace& ws,
    const std::vector<caffe2::TIndex>& shape,
    const T value,
    const std::string& name) {
  auto context = makeContext<Caffe2Backend>();
  auto* tensor = NewTensor<typename Caffe2Backend::Tensor>(ws, shape, name);
  caffe2::math::Set<T, typename Caffe2Backend::Context>(
      tensor->size(), value, tensor->template mutable_data<T>(), context.get());
  context->FinishDeviceComputation();
}

// May need copies because RNG on CPU and GPU do not produce the same
// values when initialized with the same seed.
template <
    typename Caffe2SourceBackend,
    typename Caffe2DestinationBackend,
    typename T>
void AddCopyOfTensor(
    caffe2::Workspace& ws,
    const std::string& name,
    const caffe2::Workspace& sourceWs,
    const std::string& sourceName) {
  auto sourceContext = makeContext<Caffe2SourceBackend>();
  auto destinationContext = makeContext<Caffe2DestinationBackend>();
  const auto& sourceTensor =
      sourceWs.GetBlob(sourceName)->Get<typename Caffe2SourceBackend::Tensor>();
  auto* destinationTensor =
      NewTensor<typename Caffe2DestinationBackend::Tensor>(
          ws, sourceTensor.dims(), name);
  destinationTensor->CopyFrom(sourceTensor);
  sourceContext->FinishDeviceComputation();
  destinationContext->FinishDeviceComputation();
}

template <typename Caffe2Backend, typename T>
void AddDeterministicallyRandomInputWithRange(
    caffe2::Workspace& ws,
    const std::vector<caffe2::TIndex>& shape,
    const std::string& name,
    T min,
    T max) {
  std::lock_guard<std::mutex> lock{rng_mutex};
  DeviceOption option;
  option.set_random_seed(std::hash<std::string>()(name));
  auto context = makeContext<Caffe2Backend>(option);
  auto* tensor = NewTensor<typename Caffe2Backend::Tensor>(ws, shape, name);
  caffe2::math::RandUniform<T, typename Caffe2Backend::Context>(
      tensor->size(),
      min,
      max,
      tensor->template mutable_data<T>(),
      context.get());
  context->FinishDeviceComputation();
}

template <typename Caffe2Backend, typename T>
void AddDeterministicallyRandomInput(
    caffe2::Workspace& ws,
    const std::vector<caffe2::TIndex>& shape,
    const std::string& name) {
  // 0..2 seems like a nice range for weights
  AddDeterministicallyRandomInputWithRange<Caffe2Backend, T>(
      ws, shape, name, 0, 2);
}

void CheckEqual(
    const caffe2::Tensor<caffe2::CPUBackend::Context>& Texpected,
    const caffe2::Tensor<caffe2::CPUBackend::Context>& Ttested,
    float relativePrecision = 0.0,
    long offsetInExpected = 0,
    long offsetInTested = 0);

template <typename T = caffe2::CUDABackend::Tensor>
void CheckEqual(
    const caffe2::Workspace& expected,
    const caffe2::Workspace& tested,
    const std::string& name,
    float relativePrecision = 0.0,
    long offsetInExpected = 0,
    long offsetInTested = 0) {
  // Resolved dynamically
  caffe2::CPUBackend::Tensor Texpected(expected.GetBlob(name)->Get<T>());
  caffe2::CPUBackend::Tensor Ttested(tested.GetBlob(name)->Get<T>());
  CheckEqual(
      Texpected, Ttested, relativePrecision, offsetInExpected, offsetInTested);
}

class OpTester {
  std::unique_ptr<NetBase> net_ref;
  OperatorDef op_def;
  float relativePrecision;

 public:
  Workspace w_ref;
  Workspace w_test;
  unique_ptr<OperatorBase> op_test;

  OpTester(const OperatorDef& op_def, float relativePrecision = 0.0)
      : op_def{op_def}, relativePrecision{relativePrecision} {}

  void InitializeReference(
      std::function<void(Workspace&)> ws_init_func,
      std::map<string, int> reference_args = {}) {
    ws_init_func(w_ref);
    NetDef net_def;
    caffe2::ReferenceImplementationRegistry::Append(&net_def, op_def);
    for (auto s : reference_args) {
      auto arg = net_def.mutable_op()->Mutable(0)->add_arg();
      arg->set_name(s.first);
      arg->set_i(s.second);
    }
    net_ref = CreateNet(net_def, &w_ref);
  }

  void RunReference() {
    ASSERT_TRUE(net_ref.get());
    tc::CudaProfiler p;
    ASSERT_TRUE(net_ref->Run());
  }

  void InitializeTestedOp(std::function<void(Workspace&)> ws_init_func) {
    ws_init_func(w_test);
    op_test = CreateOperator(op_def, &w_test);
  }

  void Run() {
    ASSERT_TRUE(op_test.get());
    tc::CudaProfiler p;
    ASSERT_TRUE(op_test->Run());
  }

  void Check() const {
    for (auto out : op_def.output()) {
      CheckEqual(w_ref, w_test, out, relativePrecision);
    }
  }
};

// Compares individual operator
unique_ptr<OpTester> BasicCorrectnessTest(
    const OperatorDef& op_def,
    std::function<void(Workspace&)> ws_init_func,
    float relativePrecision = 0.0,
    std::map<string, int> reference_args = {});

// Compares the entire Net and all intermediate blobs
void BasicCorrectnessTest(
    const NetDef& net_def,
    std::function<void(Workspace&)> ws_init_func,
    float relativePrecision = 0.0);

// Runs the gradient of an operator and adds the gradient tensors to the
// workspace
void RunGradient(Workspace& w, const OperatorDef& def);

/// This function runs forward and gradient for op_def (the tested operator
/// we want to compare against a reference) and for the reference
/// implementation.
/// Then it compares named tensors from both the reference and tested
/// workspace to check correctness.
///
/// op_def is the OperatorDef corresponding to the operator we wish to check
///        for correctness
/// ws_init_func is a function to initialize both the reference and tested
///        workspaces with
/// params is a map containing constexpr values for operator specific
///        parameters (e.g. strides for convolutions)
/// names_to_compare contains the names of the tensors that will be compared
///        after the gradient is run. Note tht Caffe2 seems to append the
///        _grad suffix to input tensors. For instance the gradient of
///        tensor I is I_grad. While unsatisfactory from a static robustness
///        perspective, it should be enough for testing
/// make_reference_impl is a function that builds the reference
///        implementation to compare against (see the destription of the
///        type MakeDefaultReferenceImplementationBuilder above)
template <typename Backend>
void BasicGradientCorrectnessTest(
    const OperatorDef& op_def,
    std::function<void(Workspace&)> ws_init_func,
    float relativePrecision = 0.0,
    const std::vector<std::string>& names_to_compare = {},
    std::map<string, int> params = {},
    ReferenceImplementationBuilder make_reference_impl =
        MakeDefaultReferenceImplementationBuilder());

} // namespace caffe2

#include "test_harness-inl.h"
