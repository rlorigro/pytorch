#pragma once

#include <vector>
#include <ATen/core/dispatch/OpSchemaRegistration.h>
#include <ATen/core/dispatch/KernelRegistration.h>
#include <ATen/core/function_schema.h>

namespace caffe2 {
namespace detail {

constexpr const char* PREALLOCATED_OUTPUT_ARGNAME =
    "_caffe2_preallocated_outputs";

using _CallCaffe2OpFunc = std::vector<at::Tensor>(
    const c10::FunctionSchema& schema,
    std::vector<c10::IValue>&& inputs,
    std::vector<at::Tensor>&& outputs);

template <class Caffe2Operator>
inline std::vector<at::Tensor> _call_caffe2_op(
    const c10::FunctionSchema& schema,
    std::vector<c10::IValue>&& inputs,
    std::vector<at::Tensor>&& outputs) {
  Caffe2Operator op(schema, std::move(inputs), std::move(outputs));
  op.Run();
  return std::move(op).move_newstyle_outputs();
}

// This function is inline in the hope that compilers optimizing for speed will
// inline it into call_caffe2_op_from_c10, allowing call_op to be inlined and
// avoiding the function pointer indirection, while compilers optimizing for
// binary size will keep it a separate function instead of inlining it into
// a template and will reuse the binary code of this function between ops.
// We measured and confirmed that binary size off the instagram ios app is
// reduced when having _call_caffe2_op_from_c10 separate from the templated
// call_caffe2_op_from_c10.
inline void _call_caffe2_op_from_c10(
    c10::Stack* stack,
    const c10::FunctionSchema& schema,
    _CallCaffe2OpFunc* call_op) {
  // precondition: on the stack, there's one IValue for each argument of the
  // c10 schema. The last argument is an optional tensor list that
  // (if not ivalue::None) contains a preallocated output tensor for each
  // operator output.

  AT_ASSERT(
      schema.arguments().size() != 0 &&
      schema.arguments().back().type()->isSubtypeOf(
          OptionalType::create(ListType::ofTensors())));
  IValue preallocated_outputs = torch::jit::pop(*stack);

  const size_t num_outputs = schema.returns().size();
  const size_t num_inputs = schema.arguments().size() -
      1; // -1 because the last argument is the list of preallocated tensors

  std::vector<at::Tensor> outputs;
  if (preallocated_outputs.isNone()) {
    // either the schema doesn't support preallocated outputs or it does but
    // they haven't been passed in. Pass a list of uninitialized tensors to
    // the caffe2 operator as preallocated outputs.
    outputs.resize(num_outputs);
  } else {
    AT_ASSERT(preallocated_outputs.isTensorList());
    outputs =
        std::move(*std::move(preallocated_outputs).toTensorList()).elements();
  }

  // TODO Avoid vector allocation. One idea would be to keep the std::vector
  // instances in the cache.
  std::vector<IValue> inputs = torch::jit::pop(*stack, num_inputs);

  outputs = (*call_op)(schema, std::move(inputs), std::move(outputs));

  for (auto&& output : std::move(outputs)) {
    torch::jit::push(*stack, std::move(output));
  }

  // postcondition: All inputs are cleared from the stack, there's now one
  //                IValue for each output which holds the result. This
  //                might reuse one of the preallocated tensors but doesn't have to.
}

template <const c10::OperatorHandle& (*OpHandle)(), class Caffe2Operator>
void call_caffe2_op_from_c10(
    c10::Stack* stack,
    c10::KernelCache* cache) { // TODO Pass in correct cache type
  _call_caffe2_op_from_c10(
      stack, OpHandle().schema(), &_call_caffe2_op<Caffe2Operator>);
}

inline c10::FunctionSchema make_function_schema_for_c10(const char* OperatorName, std::vector<c10::Argument> inputs, std::vector<c10::Argument> outputs) {
  // actual_inputs is the real inputs plus an optional tensor list argument
  // for preallocated outputs
  std::vector<c10::Argument> actual_inputs = std::move(inputs);
  actual_inputs.emplace_back(
      PREALLOCATED_OUTPUT_ARGNAME,
      c10::OptionalType::create(c10::ListType::ofTensors()),
      nullopt,
      IValue());

  return c10::FunctionSchema(
      std::string("_caffe2::") + OperatorName,
      "",
      std::move(actual_inputs),
      std::move(outputs));
}

}
}


/**
 * To register a caffe2 operator caffe2::MyOperator with the c10 dispatcher,
 * call:
 *
 * In caffe2/operators/MyOperator.h:
 *
 * > C10_DECLARE_CAFFE2_OPERATOR(C10MyOperator) // C10MyOperator is the name
 *                                              // used by c10 for this operator
 *
 * In caffe2/operators/MyOperator.cc
 *
 * > C10_REGISTER_CAFFE2_OPERATOR_CPU(
 * >    C10MyOperator,
 * >    (std::vector<c10::Argument>{
 * >      c10::Argument("input1"),
 * >      c10::Argument("argument2", c10::IntType::get()),
 * >      c10::Argument("argument3", c10::FloatType::get())
 * >    }), (std::vector<c10::Argument>{
 * >      c10::Argument("output1"),
 * >      c10::Argument("output2")
 * >    }),
 * >    caffe2::MyOperator<caffe2::CPUContext> // This is the caffe2 operator
 * >                                           // class template
 * > )
 *
 * In caffe2/operators/MyOperator.cu
 *
 * > C10_REGISTER_CAFFE2_OPERATOR_CUDA(C10MyOperator,
 *   caffe2::MyOperator<caffe2::CUDAContext>)
 *
 * Notes:
 * - all macros must be defined in the top level namespace, not in namespace
 *   caffe2.
 * - all operators must call C10_DECLARE_CAFFE2_OPERATOR and
 *   C10_REGISTER_CAFFE2_OPERATOR_CPU.
 * - calling C10_REGISTER_CAFFE2_OPERATOR_CUDA is optional and can be omitted if
 *   you don't want to expose the operator for CUDA operations.
 * - caffe2 arguments must come after caffe2 inputs, in other words, any tensor
 *   inputs must precede any non-tensor inputs.
 *
 * More complex use cases:
 * - If your operator has a variable number of input tensors, make the first (!)
 *   input an input of type TensorList. There must be no other tensor inputs.
 */
#ifndef C10_MOBILE
#define C10_DECLARE_CAFFE2_OPERATOR(OperatorName) \
  namespace caffe2 {                              \
  namespace _c10_ops {                            \
  C10_DECLARE_OP_SCHEMA(OperatorName);            \
  }                                               \
  }

// TODO This macro should take a JIT schema string instead of a vector of inputs and outputs.
#define C10_REGISTER_CAFFE2_OPERATOR_CPU(                                     \
    OperatorName, Inputs, Outputs, OperatorClass)                             \
  /* Register the op schema with the c10 dispatcher */                        \
  namespace caffe2 {                                                          \
  namespace _c10_ops {                                                        \
  C10_DEFINE_OP_SCHEMA(                                                       \
      OperatorName,                                                           \
      caffe2::detail::make_function_schema_for_c10(                           \
          #OperatorName,                                                      \
          Inputs,                                                             \
          Outputs));                                                          \
  }                                                                           \
  }                                                                           \
  /* Register call_caffe2_op_from_c10 as a kernel with the c10 dispatcher */  \
  namespace c10 {                                                             \
  C10_REGISTER_KERNEL(caffe2::_c10_ops::OperatorName) /*.withCache<Cache>()*/ \
      .kernel<&caffe2::detail::call_caffe2_op_from_c10<                       \
          ::caffe2::_c10_ops::OperatorName,                                   \
          OperatorClass>>()                                                   \
      .dispatchKey(CPUTensorId());                                            \
  }

#define C10_REGISTER_CAFFE2_OPERATOR_CUDA(OperatorName, OperatorClass)        \
  namespace c10 {                                                             \
  C10_REGISTER_KERNEL(caffe2::_c10_ops::OperatorName) /*.withCache<Cache>()*/ \
      .kernel<&caffe2::detail::call_caffe2_op_from_c10<                       \
          ::caffe2::_c10_ops::OperatorName,                                   \
          OperatorClass>>()                                                   \
      .dispatchKey(CUDATensorId());                                           \
  }

// You should never manually call the C10_REGISTER_CAFFE2_OPERATOR_HIP macro.
// The C10_REGISTER_CAFFE2_OPERATOR_CUDA macro from above will be automatically
// rewritten to C10_REGISTER_CAFFE2_OPERATOR_HIP by hipify.
#define C10_REGISTER_CAFFE2_OPERATOR_HIP(OperatorName, OperatorClass)         \
  namespace c10 {                                                             \
  C10_REGISTER_KERNEL(caffe2::_c10_ops::OperatorName) /*.withCache<Cache>()*/ \
      .kernel<&caffe2::detail::call_caffe2_op_from_c10<                       \
          ::caffe2::_c10_ops::OperatorName,                                   \
          OperatorClass>>()                                                   \
      .dispatchKey(HIPTensorId());                                            \
  }

#else
// Don't use c10 dispatcher on mobile because of binary size
#define C10_DECLARE_CAFFE2_OPERATOR(OperatorName)
#define C10_REGISTER_CAFFE2_OPERATOR_CPU(OperatorName, Inputs, Outputs, OperatorClass)
#define C10_REGISTER_CAFFE2_OPERATOR_CUDA(OperatorName, OperatorClass)
#define C10_REGISTER_CAFFE2_OPERATOR_HIP(OperatorName, OperatorClass)
#endif
