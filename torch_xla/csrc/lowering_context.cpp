#include "torch_xla/csrc/lowering_context.h"

#include <sstream>
#include <stdexcept>

#include "tensorflow/compiler/xla/xla_client/debug_macros.h"

namespace torch_xla {
namespace ir {

LoweringContext::LoweringContext(const std::string& name) : builder_(name) {}

xla::XlaOp LoweringContext::GetParameter(
    const std::shared_ptr<xla::ComputationClient::Data>& data) {
  auto it = parameters_map_.find(data.get());
  if (it == parameters_map_.end()) {
    xla::XlaOp param =
        xla::Parameter(builder(), parameters_.size(), data->shape(),
                       absl::StrCat("param_", parameters_.size()));
    parameters_.push_back(data);
    it = parameters_map_.emplace(data.get(), param).first;
  }
  return it->second;
}

std::vector<xla::ComputationClient::Data*> LoweringContext::GetParametersData()
    const {
  std::vector<xla::ComputationClient::Data*> parameters;
  for (auto& param : parameters_) {
    parameters.push_back(param.get());
  }
  return parameters;
}

xla::int64 LoweringContext::AddResult(xla::XlaOp op) {
  root_tuple_.push_back(std::move(op));
  return root_tuple_.size() - 1;
}

xla::StatusOr<xla::XlaComputation> LoweringContext::Build() {
  if (!root_tuple_.empty()) {
    xla::XlaOp root = xla::Tuple(builder(), root_tuple_);
    return builder()->Build(root);
  }
  return builder()->Build();
}

xla::StatusOr<xla::XlaComputation> LoweringContext::Build(
    const xla::XlaOp& root) {
  XLA_CHECK(root_tuple_.empty());
  return builder()->Build(root);
}

void LoweringContext::AssignOutputOp(const Output& output, xla::XlaOp op) {
  emitted_outputs_[output] = op;
}

xla::XlaOp LoweringContext::GetOutputOp(const Output& output) {
  auto it = emitted_outputs_.find(output);
  if (it == emitted_outputs_.end()) {
    auto post_order = Util::ComputePostOrder(output.node, &emit_status_);
    LowerPostOrder(post_order);
    // At this point the outpout better be present, otherwise there is an issue
    // with the lowering code.
    it = emitted_outputs_.find(output);
    XLA_CHECK(it != emitted_outputs_.end())
        << "No XLA operation emitted for output: " << output;
  }
  return it->second;
}

void LoweringContext::LowerPostOrder(
    tensorflow::gtl::ArraySlice<const Node* const> post_order) {
  for (auto node : post_order) {
    try {
      node->Lower(this);
    } catch (const std::exception& ex) {
      ReportBuilderError(node, ex.what());
    }
    if (!builder()->first_error().ok()) {
      ReportBuilderError(node, /*error_msg=*/nullptr);
    }
  }
}

void LoweringContext::ReportBuilderError(const Node* node,
                                         const char* error_msg) {
  std::stringstream ss;
  ss << "Error while lowering: " << node->ToString() << "\n";
  if (!builder()->first_error().ok()) {
    ss << "XLA builder error: " << builder()->GetCurrentStatus() << "\n";
  }
  if (error_msg != nullptr) {
    ss << "Error: " << error_msg << "\n";
  }
  ss << node->metadata().frame_info;
  throw std::runtime_error(ss.str());
}

}  // namespace ir
}  // namespace torch_xla
