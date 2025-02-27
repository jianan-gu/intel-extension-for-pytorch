// Copyright (c) Facebook, Inc. and its affiliates. All Rights Reserved.
#include "ROIAlign.h"
#include <ATen/ATen.h>
#include <ATen/Parallel.h>
#include <ATen/cpu/vec/vec.h>
#include <torch/library.h>
#include "csrc/autocast/autocast_mode.h"
#include "csrc/utils/ipex_op_profile.h"
#include "csrc/utils/library.h"

namespace torch_ipex {
namespace cpu {

DEFINE_DISPATCH(roi_align_forward_kernel_stub);
DEFINE_DISPATCH(roi_align_backward_kernel_stub);

at::Tensor IPEXROIAlignOp::_forward(
    const at::Tensor& input,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t sampling_ratio,
    bool aligned) {
  IPEX_RECORD_FUNCTION(
      "IPEXROIAlignOp::_forward", std::vector<c10::IValue>({}));

  /*
  pointer to roi_align_forward_kernel_impl(
      input,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      sampling_ratio,
      aligned);
  */
  return roi_align_forward_kernel_stub(
      kCPU,
      input,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      sampling_ratio,
      aligned);
}

at::Tensor IPEXROIAlignOp::forward(
    torch::autograd::AutogradContext* ctx,
    const at::Tensor& input,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t sampling_ratio,
    bool aligned) {
  IPEX_RECORD_FUNCTION("IPEXROIAlignOp::forward", std::vector<c10::IValue>({}));

  ctx->saved_data["input_shape"] = input.sizes();
  ctx->saved_data["spatial_scale"] = spatial_scale;
  ctx->saved_data["pooled_height"] = pooled_height;
  ctx->saved_data["pooled_width"] = pooled_width;
  ctx->saved_data["sampling_ratio"] = sampling_ratio;
  ctx->saved_data["aligned"] = aligned;
  ctx->saved_data["is_channels_last"] =
      input.is_contiguous(at::MemoryFormat::ChannelsLast);
  ctx->save_for_backward({rois});

  /*
  pointer to roi_align_forward_kernel_impl(
      input,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      sampling_ratio,
      aligned);
  */
  return roi_align_forward_kernel_stub(
      kCPU,
      input,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      sampling_ratio,
      aligned);
}

torch::autograd::variable_list IPEXROIAlignOp::backward(
    torch::autograd::AutogradContext* ctx,
    torch::autograd::variable_list grad_outputs) {
  IPEX_RECORD_FUNCTION(
      "IPEXROIAlignOp::backward", std::vector<c10::IValue>({}));

  auto input_shape = ctx->saved_data["input_shape"].toIntVector();
  auto spatial_scale = ctx->saved_data["spatial_scale"].toDouble();
  auto pooled_height = ctx->saved_data["pooled_height"].toInt();
  auto pooled_width = ctx->saved_data["pooled_width"].toInt();
  auto sampling_ratio = ctx->saved_data["sampling_ratio"].toInt();
  auto aligned = ctx->saved_data["aligned"].toBool();
  auto is_channels_last = ctx->saved_data["is_channels_last"].toBool();
  auto saved = ctx->get_saved_variables();
  at::Tensor rois = saved[0];

  /*
  pointer to roi_align_backward_kernel_impl(
      grad_outputs[0],
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      input_shape[0],
      input_shape[1],
      input_shape[2],
      input_shape[3],
      sampling_ratio,
      aligned,
      is_channels_last);
  */
  at::Tensor grad_input = roi_align_backward_kernel_stub(
      kCPU,
      grad_outputs[0],
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      input_shape[0],
      input_shape[1],
      input_shape[2],
      input_shape[3],
      sampling_ratio,
      aligned,
      is_channels_last);

  return {
      grad_input,
      at::Tensor(),
      at::Tensor(),
      at::Tensor(),
      at::Tensor(),
      at::Tensor(),
      at::Tensor()};
}

at::Tensor ROIAlign_forward(
    const at::Tensor& input,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t sampling_ratio,
    bool aligned) {
  if (at::GradMode::is_enabled()) {
    return IPEXROIAlignOp::apply(
        input,
        rois,
        spatial_scale,
        pooled_height,
        pooled_width,
        sampling_ratio,
        aligned);
  }
  return IPEXROIAlignOp::_forward(
      input,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      sampling_ratio,
      aligned);
}

at::Tensor roi_align_forward_kernel_instance(
    const at::Tensor& input,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t sampling_ratio,
    bool aligned) {
  /*
  pointer to roi_align_forward_kernel_impl(
      input,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      sampling_ratio,
      aligned);
  */
  return roi_align_forward_kernel_stub(
      kCPU,
      input,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      sampling_ratio,
      aligned);
}

at::Tensor roi_align_backward_kernel_instance(
    const at::Tensor& grad,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t batch_size,
    int64_t channels,
    int64_t height,
    int64_t width,
    int64_t sampling_ratio,
    bool aligned,
    bool is_channels_last) {
  /*
  pointer to roi_align_backward_kernel_impl(
      grad,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      batch_size,
      channels,
      height,
      width,
      sampling_ratio,
      aligned,
      is_channels_last);
  */
  return roi_align_backward_kernel_stub(
      kCPU,
      grad,
      rois,
      spatial_scale,
      pooled_height,
      pooled_width,
      batch_size,
      channels,
      height,
      width,
      sampling_ratio,
      aligned,
      is_channels_last);
}

} // namespace cpu
} // namespace torch_ipex

namespace torch_ipex {
namespace autocast {

at::Tensor roi_align_autocast(
    const at::Tensor& input,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t sampling_ratio,
    bool aligned) {
  c10::impl::ExcludeDispatchKeyGuard no_autocastCPU(DispatchKey::AutocastCPU);
  static auto op = torch::Dispatcher::singleton()
                       .findSchemaOrThrow("torchvision::roi_align", "")
                       .typed<decltype(torch_ipex::cpu::ROIAlign_forward)>();
  if (input.scalar_type() == at::ScalarType::BFloat16) {
    return op.call(
        input,
        cpu_cached_cast(at::kFloat, rois),
        spatial_scale,
        pooled_height,
        pooled_width,
        sampling_ratio,
        aligned);
  } else {
    return op.call(
        input,
        cpu_cached_cast(input.scalar_type(), rois),
        spatial_scale,
        pooled_height,
        pooled_width,
        sampling_ratio,
        aligned);
  }
}

at::Tensor ROIAlign_forward(
    const at::Tensor& input,
    const at::Tensor& rois,
    double spatial_scale,
    int64_t pooled_height,
    int64_t pooled_width,
    int64_t sampling_ratio,
    bool aligned) {
  c10::impl::ExcludeDispatchKeyGuard no_autocastCPU(DispatchKey::AutocastCPU);
  static auto op = torch::Dispatcher::singleton()
                       .findSchemaOrThrow("torch_ipex::ROIAlign_forward", "")
                       .typed<decltype(torch_ipex::cpu::ROIAlign_forward)>();
  if (input.scalar_type() == at::ScalarType::BFloat16) {
    return op.call(
        input,
        cpu_cached_cast(at::kFloat, rois),
        spatial_scale,
        pooled_height,
        pooled_width,
        sampling_ratio,
        aligned);
  } else {
    return op.call(
        input,
        cpu_cached_cast(input.scalar_type(), rois),
        spatial_scale,
        pooled_height,
        pooled_width,
        sampling_ratio,
        aligned);
  }
}

} // namespace autocast
} // namespace torch_ipex

namespace {

IPEX_TORCH_LIBRARY_FRAGMENT(torch_ipex, m) {
  m.def(
      "ROIAlign_forward(Tensor input, Tensor rois, float spatial_scale, int pooled_height, int pooled_width, int sampling_ratio, bool aligned) -> Tensor");
  m.impl(
      "ROIAlign_forward",
      c10::DispatchKey::AutogradCPU,
      torch_ipex::cpu::ROIAlign_forward);
  m.impl(
      "ROIAlign_forward",
      c10::DispatchKey::AutocastCPU,
      torch_ipex::autocast::ROIAlign_forward);
  m.impl(
      "ROIAlign_forward",
      c10::DispatchKey::CPU,
      torch_ipex::cpu::roi_align_forward_kernel_instance);
  // bw
  m.def(
      "_ROIAlign_backward(Tensor grad, Tensor rois, float spatial_scale, int pooled_height, int pooled_width, int batch_size, int channels, int height, int width, int sampling_ratio, bool aligned, bool is_channels_last) -> Tensor");
  m.impl(
      "_ROIAlign_backward",
      c10::DispatchKey::CPU,
      torch_ipex::cpu::roi_align_backward_kernel_instance);
}

IPEX_TORCH_LIBRARY_FRAGMENT(torchvision, m) {
  m.impl(
      "roi_align",
      c10::DispatchKey::AutogradCPU,
      torch_ipex::cpu::ROIAlign_forward);
  m.impl(
      "roi_align",
      c10::DispatchKey::AutocastCPU,
      torch_ipex::autocast::roi_align_autocast);
}

} // namespace