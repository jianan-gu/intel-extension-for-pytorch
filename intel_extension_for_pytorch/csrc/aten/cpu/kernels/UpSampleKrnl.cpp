#include <ATen/ATen.h>
#include <ATen/Dispatch.h>
#include <ATen/NativeFunctions.h>
#include <ATen/Parallel.h>
#include <ATen/cpu/vec/vec.h>
#include <ATen/native/TensorIterator.h>
#include <ATen/native/UpSample.h>
#include <ATen/native/cpu/utils.h>
#include <ATen/record_function.h>
#include <c10/util/accumulate.h>

#include <math.h>
#include <vector>

#include <csrc/aten/cpu/UpSample.h>

#include "csrc/utils/library.h"

namespace torch_ipex {
namespace cpu {

namespace {

using scale_t = std::vector<c10::optional<double>>;

static inline int64_t nearest_idx(
    int64_t output_index,
    int64_t input_size,
    int64_t output_size,
    c10::optional<double> scales) {
  if (output_size == input_size) {
    // scale_factor = 1, simply copy
    return output_index;
  } else if (output_size == 2 * input_size) {
    // scale_factor = 2, shift input index
    return output_index >> 1;
  } else {
    float scale = at::native::compute_scales_value<float>(
        scales, input_size, output_size);
    return at::native::nearest_neighbor_compute_source_index(
        scale, output_index, input_size);
  }
}

// Helper structs and methods for cpu_upsample_linear
//
// Interpolation methods that used below are separable, and as such we can
// compute the interpolation independently per dimension in a recursive way.
// Please, refer to #10482 for more context.
//
// Linear Interpolation structure to compute output value in n-dimensional case.
// - recursively compute interpolated output for each dimension
// - we rely a lot on compiler's code optimization such that implemented
// operations
//   can be automatically factorized and vectorized using SSE and AVX2
template <int n, typename scalar_t, typename index_t, int interp_size>
struct Interpolate {
  static inline scalar_t eval(
      char* src,
      char** data,
      const int64_t* strides,
      int64_t i) {
    index_t ids = *(index_t*)&data[0][i * strides[0]];
    scalar_t wts = *(scalar_t*)&data[1][i * strides[1]];
    scalar_t t = Interpolate<n - 1, scalar_t, index_t, interp_size>::eval(
        src + ids, &data[2 * interp_size], &strides[2 * interp_size], i);
    scalar_t output = t * wts;
    for (int j = 1; j < interp_size; j++) {
      ids = *(index_t*)&data[2 * j + 0][i * strides[2 * j + 0]];
      wts = *(scalar_t*)&data[2 * j + 1][i * strides[2 * j + 1]];
      t = Interpolate<n - 1, scalar_t, index_t, interp_size>::eval(
          src + ids, &data[2 * interp_size], &strides[2 * interp_size], i);
      output += t * wts;
    }
    return output;
  }
};

template <typename scalar_t, typename index_t, int interp_size>
struct Interpolate<1, scalar_t, index_t, interp_size> {
  static inline scalar_t eval(
      char* src,
      char** data,
      const int64_t* strides,
      int64_t i) {
    index_t ids = *(index_t*)&data[0][i * strides[0]];
    scalar_t wts = *(scalar_t*)&data[1][i * strides[1]];
    scalar_t t = *(scalar_t*)&src[ids];
    scalar_t output = t * wts;
    for (int j = 1; j < interp_size; j++) {
      ids = *(index_t*)&data[2 * j + 0][i * strides[2 * j + 0]];
      wts = *(scalar_t*)&data[2 * j + 1][i * strides[2 * j + 1]];
      t = *(scalar_t*)&src[ids];
      output += t * wts;
    }
    return output;
  }
};

template <int n, typename scalar_t, typename index_t>
struct Interpolate<n, scalar_t, index_t, 1> {
  static inline scalar_t eval(
      char* src,
      char** data,
      const int64_t* strides,
      int64_t i) {
    index_t ids = *(index_t*)&data[0][i * strides[0]];
    return Interpolate<n - 1, scalar_t, index_t, 1>::eval(
        src + ids, &data[2], &strides[2], i);
  }
};

template <typename scalar_t, typename index_t>
struct Interpolate<1, scalar_t, index_t, 1> {
  static inline scalar_t eval(
      char* src,
      char** data,
      const int64_t* strides,
      int64_t i) {
    index_t ids = *(index_t*)&data[0][i * strides[0]];
    return *(scalar_t*)&src[ids];
  }
};

// There is an unexpected 2x slowdown for upsample_trilinear3d channels_first
// for both 1 and 6 threads. We have to specialize this case as below:
// Once the issue is fixed we can keep generic implementation and remove:
// struct Interpolate<n, scalar_t, index_t, 2> and
// struct Interpolate<1, scalar_t, index_t, 2>
template <int n, typename scalar_t, typename index_t>
struct Interpolate<n, scalar_t, index_t, 2> {
  static inline scalar_t eval(
      char* src,
      char** data,
      const int64_t* strides,
      int64_t i) {
    index_t i0 = *(index_t*)&data[0][i * strides[0]];
    index_t i1 = *(index_t*)&data[2][i * strides[2]];
    scalar_t w0 = *(scalar_t*)&data[1][i * strides[1]];
    scalar_t w1 = *(scalar_t*)&data[3][i * strides[3]];

    scalar_t t0 = Interpolate<n - 1, scalar_t, index_t, 2>::eval(
        src + i0, &data[4], &strides[4], i);
    scalar_t t1 = Interpolate<n - 1, scalar_t, index_t, 2>::eval(
        src + i1, &data[4], &strides[4], i);

    return t0 * w0 + t1 * w1;
  }
};

template <typename scalar_t, typename index_t>
struct Interpolate<1, scalar_t, index_t, 2> {
  static inline scalar_t eval(
      char* src,
      char** data,
      const int64_t* strides,
      int64_t i) {
    index_t i0 = *(index_t*)&data[0][i * strides[0]];
    index_t i1 = *(index_t*)&data[2][i * strides[2]];
    scalar_t w0 = *(scalar_t*)&data[1][i * strides[1]];
    scalar_t w1 = *(scalar_t*)&data[3][i * strides[3]];
    scalar_t t0 = *(scalar_t*)&src[i0];
    scalar_t t1 = *(scalar_t*)&src[i1];
    return t0 * w0 + t1 * w1;
  }
};

template <int n, typename scalar_t, typename index_t, int interp_size>
static inline scalar_t interpolate(
    char* src,
    char** data,
    const int64_t* strides,
    int64_t i) {
  return Interpolate<n, scalar_t, index_t, interp_size>::eval(
      src, data, strides, i);
}

template <int interp_size>
static inline bool is_zero_stride(const int64_t* strides) {
  bool output = strides[0] == 0;
  for (int i = 1; i < 2 * interp_size; i++) {
    output &= (strides[i] == 0);
  }
  return output;
}

template <typename scalar_t, typename index_t, int interp_size>
static inline bool is_contiguous_stride(const int64_t* strides) {
  bool output =
      (strides[0] == sizeof(index_t)) && (strides[1] == sizeof(scalar_t));
  for (int i = 2; i < 2 * interp_size; i += 2) {
    output &=
        (strides[i] == sizeof(index_t)) && (strides[i + 1] == sizeof(scalar_t));
  }
  return output;
}

// Helper class to recursively check if all input strides corresponding to
// interpolated dimensions are equal zero except on a single dimension.
//
// Inputs: array of strides of size N, non_zero_stride_dim which can be -1, 0,
// 1, 2, ...
//   if non_zero_stride_dim, we check that all strides are equal zero, otherwise
//   4 strides corresponding to the strides for index_0, weight_0, index_1 and
//   weight_1 for non_zero_stride_dim dimension should be non zero.
//
// Unit check of the recursion is to verify whether 4 strides for one
// interpolated dimension are either zero, see method is_zero_stride, or
// (sizeof(index_t), sizeof(scalar_t), sizeof(index_t), sizeof(scalar_t)), see
// method is_contiguous_stride.
//
// In practice, we have the following cases:
// - for ND, float32, channel first, strides are
//         dimN-1,              dim1,           dim0
//         i0, w0, i1, w1, ..., i0, w0, i1, w1, i0, w0, i1, w1
// strides=(0,  0,  0,  0, ...,  0,  0,  0,  0,  4,  4,  4,  4)
//
// if size dim0 is 1 then its strides are 0 and dim1 strides are equal 4
//
// - for ND, float32, channel last, strides are
//         dimN-1,         dimN-2,             dim0
//         i0, w0, i1, w1, i0, w0, i1, w1, ... i0, w0, i1, w1
// strides=(0,  0,  0,  0,  0,  0,  0,  0, ..., 0,  0,  0,  0)
//
// Using these methods we can hint the compiler to factorize constant indices
// and weights in cpu_upsample_linear method
template <
    int N,
    int non_zero_stride_dim,
    typename scalar_t,
    typename index_t,
    int interp_size>
struct CheckAlmostAllZeroStrides {
  static inline bool eval(const int64_t* strides) {
    // N is dim index: N -> dim0, N-1 -> dim1, ...
    // non_zero_stride_dim should be out_dims - dim
    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    bool output;
    if (N == non_zero_stride_dim) {
      output = is_contiguous_stride<scalar_t, index_t, interp_size>(strides);
    } else {
      output = is_zero_stride<interp_size>(strides);
    }
    return output &&
        CheckAlmostAllZeroStrides<
               N - 1,
               non_zero_stride_dim,
               scalar_t,
               index_t,
               interp_size>::eval(&strides[2 * interp_size]);
  }
};

template <
    int non_zero_stride_dim,
    typename scalar_t,
    typename index_t,
    int interp_size>
struct CheckAlmostAllZeroStrides<
    0,
    non_zero_stride_dim,
    scalar_t,
    index_t,
    interp_size> {
  static inline bool eval(const int64_t* strides) {
    return true;
  }
};

template <int n, int s, typename scalar_t, typename index_t, int interp_size>
static inline bool check_almost_all_zero_stride(const int64_t* strides) {
  return CheckAlmostAllZeroStrides<n, s, scalar_t, index_t, interp_size>::eval(
      strides);
}

// Helper method to compute interpolation for nearest, linear, cubic modes
template <typename scalar_t, typename index_t, int out_ndims, int interp_size>
static inline void basic_loop(char** data, const int64_t* strides, int64_t n) {
  char* dst = data[0];
  char* src = data[1];
  for (int64_t i = 0; i < n; i++) {
    *(scalar_t*)&dst[i * strides[0]] =
        interpolate<out_ndims, scalar_t, index_t, interp_size>(
            src + i * strides[1], &data[2], &strides[2], i);
  }
}

// Generic upsampling computation method using TensorIterator for Nd case.
// Supports: nearest, linear, cubic modes with interp_size template argument: 1,
// 2, 4
//
// Single loop function for 1d, 2d and 3d cases and modes
// For N dimensions, output value up to Di dimension can be computed as
//
// output_i[a] = interpolate(output_{i+1}[a], w_{i+1}[a], output_{i+1}[a+1],
// w_{i+1}[a+1], ...) with output_DN[a] = interpolate(input_DN[a], w_DN[a],
// input_DN[a+1], w_DN[a+1], ...) and i - dimension index and a - linear index
// for spatial coordinates
//
// The recursive call is implemented with InterpLinear struct using template for
// the loop unrolling on compile time.
template <typename scalar_t, int out_ndims, int interp_size>
void cpu_upsample_generic(at::TensorIterator& iter) {
  auto loop = [&](char** data, const int64_t* strides, int64_t n) {
    // special-cases to let the compiler apply compile-time input-specific
    // optimizations
    if ((strides[0] == sizeof(scalar_t) && (strides[1] == 0) &&
         // NOLINTNEXTLINE(bugprone-branch-clone)
         check_almost_all_zero_stride<
             out_ndims,
             1,
             scalar_t,
             int64_t,
             interp_size>(&strides[2]))) {
      // contiguous channels-first case
      basic_loop<scalar_t, int64_t, out_ndims, interp_size>(data, strides, n);
    } else if ((strides[0] == sizeof(scalar_t) &&
                (strides[1] == sizeof(scalar_t)) &&
                check_almost_all_zero_stride<
                    out_ndims,
                    -1,
                    scalar_t,
                    int64_t,
                    interp_size>(&strides[2]))) {
      // contiguous channels-last case
      basic_loop<scalar_t, int64_t, out_ndims, interp_size>(data, strides, n);
    } else {
      // fallback
      basic_loop<scalar_t, int64_t, out_ndims, interp_size>(data, strides, n);
    }
  };
  iter.for_each(loop);
}

template <typename scalar_t, typename scale_type>
void cpu_upsample_nearest_channels_last(
    const at::Tensor& output_,
    const at::Tensor& input_,
    const scale_type& scales) {
  TORCH_CHECK(
      input_.dtype() == output_.dtype(),
      "expected dtype ",
      input_.dtype(),
      " for `output` but got dtype ",
      output_.dtype());

  auto input_sizes = input_.sizes().vec();
  auto output_sizes = output_.sizes().vec();
  auto ndim = input_sizes.size();
  TORCH_CHECK(
      ndim >= 4 && ndim <= 5,
      "Upsample with NHWC format supports tensors with 4 or 5 dims.")

  auto channels_last_memory_format = ndim == 4
      ? at::MemoryFormat::ChannelsLast
      : at::MemoryFormat::ChannelsLast3d;
  auto input = input_.contiguous(channels_last_memory_format);
  auto output = output_.contiguous(channels_last_memory_format);

  auto input_data = input.data_ptr<scalar_t>();
  auto output_data = output.data_ptr<scalar_t>();

  int64_t num_batches = input_sizes[0];
  int64_t channels = input_sizes[1];
  int64_t input_depth = (ndim == 5) ? input_sizes[2] : 1;
  int64_t output_depth = (ndim == 5) ? output_sizes[2] : 1;
  int64_t input_height = (ndim >= 4) ? input_sizes[ndim - 2] : 1;
  int64_t output_height = (ndim >= 4) ? output_sizes[ndim - 2] : 1;
  int64_t input_width = input_sizes[ndim - 1];
  int64_t output_width = output_sizes[ndim - 1];
  int64_t numel = output.numel();

  TORCH_CHECK(
      channels > 0,
      "expected input and output channels greater than 0 but got ",
      channels);

  using Vec = at::vec::Vectorized<scalar_t>;
  auto copy = [](scalar_t* out, scalar_t* in, int64_t size) {
    int64_t d = 0;
    for (; d < size - (size % Vec::size()); d += Vec::size()) {
      Vec out_vec = Vec::loadu(in + d);
      out_vec.store(out + d);
    }
    for (; d < size; d++) {
      out[d] = in[d];
    }
  };

  auto loop2d = [&](int64_t begin, int64_t end) {
    int64_t n = 0;
    int64_t oh = 0;
    int64_t ow = 0;
    at::native::data_index_init(
        begin, n, num_batches, oh, output_height, ow, output_width);

    for (int64_t i = begin; i < end; i++) {
      int64_t ih = nearest_idx(oh, input_height, output_height, scales[0]);
      int64_t iw = nearest_idx(ow, input_width, output_width, scales[1]);
      scalar_t* output_ptr = output_data + i * channels;
      scalar_t* input_ptr = input_data +
          n * input_height * input_width * channels +
          ih * input_width * channels + iw * channels;
      copy(output_ptr, input_ptr, channels);
      at::native::data_index_step(
          n, num_batches, oh, output_height, ow, output_width);
    }
  };

  auto loop3d = [&](int64_t begin, int64_t end) {
    int64_t n = 0;
    int64_t od = 0;
    int64_t oh = 0;
    int64_t ow = 0;
    at::native::data_index_init(
        begin,
        n,
        num_batches,
        od,
        output_depth,
        oh,
        output_height,
        ow,
        output_width);

    for (int64_t i = begin; i < end; i++) {
      int64_t id = nearest_idx(od, input_depth, output_depth, scales[0]);
      int64_t ih = nearest_idx(oh, input_height, output_height, scales[1]);
      int64_t iw = nearest_idx(ow, input_width, output_width, scales[2]);
      scalar_t* output_ptr = output_data + i * channels;
      scalar_t* input_ptr = input_data +
          n * input_depth * input_height * input_width * channels +
          id * input_height * input_width * channels +
          ih * input_width * channels + iw * channels;
      copy(output_ptr, input_ptr, channels);
      at::native::data_index_step(
          n,
          num_batches,
          od,
          output_depth,
          oh,
          output_height,
          ow,
          output_width);
    }
  };

  if (ndim == 4) {
    // upsample nearest 2d
    at::parallel_for(
        0, numel / channels, at::internal::GRAIN_SIZE / channels, loop2d);
  } else {
    // upsample nearest 3d
    TORCH_INTERNAL_ASSERT(ndim == 5);
    at::parallel_for(
        0, numel / channels, at::internal::GRAIN_SIZE / channels, loop3d);
  }

  if (!output_.is_contiguous(channels_last_memory_format)) {
    output_.copy_(output);
  }
}

template <typename scalar_t, typename accscalar_t>
inline at::vec::Vectorized<scalar_t> interpolate(
    const scalar_t* t,
    accscalar_t w) {
  return at::vec::Vectorized<scalar_t>::loadu(t) *
      at::vec::Vectorized<scalar_t>(scalar_t(w));
}

template <typename scalar_t, typename accscalar_t, typename... Args>
inline at::vec::Vectorized<scalar_t> interpolate(
    const scalar_t* t,
    accscalar_t w,
    Args... args) {
  return at::vec::Vectorized<scalar_t>::loadu(t) *
      at::vec::Vectorized<scalar_t>(scalar_t(w)) +
      interpolate(args...);
}

// use float as immediate dtype for bfloat16 input so as to reduce rouding error
// and improve performance.
using Vec2f =
    std::tuple<at::vec::Vectorized<float>, at::vec::Vectorized<float>>;
inline Vec2f interpolate(const at::BFloat16* t, float w) {
  at::vec::Vectorized<float> t0, t1;
  std::tie(t0, t1) =
      convert_bfloat16_float(at::vec::Vectorized<at::BFloat16>::loadu(t));
  return std::make_tuple(
      t0 * at::vec::Vectorized<float>(w), t1 * at::vec::Vectorized<float>(w));
}

template <typename... Args>
inline Vec2f interpolate(const at::BFloat16* t, float w, Args... args) {
  at::vec::Vectorized<float> t0, t1;
  std::tie(t0, t1) =
      convert_bfloat16_float(at::vec::Vectorized<at::BFloat16>::loadu(t));
  at::vec::Vectorized<float> s0, s1;
  std::tie(s0, s1) = interpolate(args...);
  return std::make_tuple(
      t0 * at::vec::Vectorized<float>(w) + s0,
      t1 * at::vec::Vectorized<float>(w) + s1);
}

template <typename scalar_t>
static inline void store(scalar_t* t, const at::vec::Vectorized<scalar_t>& v) {
  v.store(t);
}

static inline void store(at::BFloat16* t, const Vec2f& v) {
  store(t, convert_float_bfloat16(std::get<0>(v), std::get<1>(v)));
}

template <typename scalar_t, typename scale_type>
void cpu_upsample_linear_channels_last(
    const at::Tensor& output_,
    const at::Tensor& input_,
    bool align_corners,
    const scale_type& scales) {
  TORCH_CHECK(
      input_.dtype() == output_.dtype(),
      "expected dtype ",
      input_.dtype(),
      " for `output` but got dtype ",
      output_.dtype());

  auto input_sizes = input_.sizes().vec();
  auto output_sizes = output_.sizes().vec();
  auto ndim = input_sizes.size();
  TORCH_CHECK(
      ndim >= 4 && ndim <= 5,
      "Upsample with NHWC format supports tensors with 4 or 5 dims.")

  auto channels_last_memory_format = ndim == 4
      ? at::MemoryFormat::ChannelsLast
      : at::MemoryFormat::ChannelsLast3d;
  auto input = input_.contiguous(channels_last_memory_format);
  auto output = output_.contiguous(channels_last_memory_format);

  auto input_data = input.data_ptr<scalar_t>();
  auto output_data = output.data_ptr<scalar_t>();

  int64_t num_batches = input_sizes[0];
  int64_t channels = input_sizes[1];
  int64_t input_depth = (ndim == 5) ? input_sizes[2] : 1;
  int64_t output_depth = (ndim == 5) ? output_sizes[2] : 1;
  int64_t input_height = (ndim >= 4) ? input_sizes[ndim - 2] : 1;
  int64_t output_height = (ndim >= 4) ? output_sizes[ndim - 2] : 1;
  int64_t input_width = input_sizes[ndim - 1];
  int64_t output_width = output_sizes[ndim - 1];

  TORCH_CHECK(
      channels > 0,
      "expected input and output channels greater than 0 but got ",
      channels);
  int64_t output_slice_size =
      output_depth * output_height * output_width * channels;

  using accscalar_t = at::acc_type<scalar_t, false>;
  using Vec = at::vec::Vectorized<scalar_t>;
  auto loop2d = [&](int64_t begin, int64_t end) {
    const scalar_t height_scale =
        at::native::area_pixel_compute_scale<scalar_t>(
            input_height, output_height, align_corners, scales[0]);
    const scalar_t width_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_width, output_width, align_corners, scales[1]);

    auto input_indexr = [=](int64_t n, int64_t h, int64_t w) {
      return input_data + n * input_height * input_width * channels +
          h * input_width * channels + w * channels;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int64_t ih0, ih1, iw0, iw1;
    scalar_t h0lambda, h1lambda, w0lambda, w1lambda;
    for (int64_t n = begin; n < end; n++) {
      for (int64_t oh = 0; oh < output_height; oh++) {
        at::native::compute_source_index_and_lambda(
            ih0,
            ih1,
            h0lambda,
            h1lambda,
            height_scale,
            oh,
            input_height,
            output_height,
            align_corners);
        for (int64_t ow = 0; ow < output_width; ow++) {
          at::native::compute_source_index_and_lambda(
              iw0,
              iw1,
              w0lambda,
              w1lambda,
              width_scale,
              ow,
              input_width,
              output_width,
              align_corners);

          scalar_t* out = output_data + n * output_slice_size +
              oh * output_width * channels + ow * channels;
          scalar_t* i00 = input_indexr(n, ih0, iw0);
          scalar_t* i01 = input_indexr(n, ih0, iw1);
          scalar_t* i10 = input_indexr(n, ih1, iw0);
          scalar_t* i11 = input_indexr(n, ih1, iw1);
          accscalar_t w00 = h0lambda * w0lambda;
          accscalar_t w01 = h0lambda * w1lambda;
          accscalar_t w10 = h1lambda * w0lambda;
          accscalar_t w11 = h1lambda * w1lambda;

          int64_t size = channels;
          int64_t d = 0;
          for (; d < size - (size % Vec::size()); d += Vec::size()) {
            auto out_vec = interpolate(
                i00 + d, w00, i01 + d, w01, i10 + d, w10, i11 + d, w11);
            store(out + d, out_vec);
          }
          for (; d < size; d++) {
            out[d] = i00[d] * w00 + i01[d] * w01 + i10[d] * w10 + i11[d] * w11;
          }
        }
      }
    }
  };

  auto loop3d = [&](int64_t begin, int64_t end) {
    const scalar_t depth_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_depth, output_depth, align_corners, scales[0]);
    const scalar_t height_scale =
        at::native::area_pixel_compute_scale<scalar_t>(
            input_height, output_height, align_corners, scales[1]);
    const scalar_t width_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_width, output_width, align_corners, scales[2]);

    auto input_indexr = [=](int64_t n, int64_t d, int64_t h, int64_t w) {
      return input_data +
          n * input_depth * input_height * input_width * channels +
          d * input_height * input_width * channels +
          h * input_width * channels + w * channels;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int64_t id0, id1, ih0, ih1, iw0, iw1;
    scalar_t d0lambda, d1lambda, h0lambda, h1lambda, w0lambda, w1lambda;
    for (int64_t n = begin; n < end; n++) {
      for (int64_t od = 0; od < output_depth; od++) {
        at::native::compute_source_index_and_lambda(
            id0,
            id1,
            d0lambda,
            d1lambda,
            depth_scale,
            od,
            input_depth,
            output_depth,
            align_corners);
        for (int64_t oh = 0; oh < output_height; oh++) {
          at::native::compute_source_index_and_lambda(
              ih0,
              ih1,
              h0lambda,
              h1lambda,
              height_scale,
              oh,
              input_height,
              output_height,
              align_corners);
          for (int64_t ow = 0; ow < output_width; ow++) {
            at::native::compute_source_index_and_lambda(
                iw0,
                iw1,
                w0lambda,
                w1lambda,
                width_scale,
                ow,
                input_width,
                output_width,
                align_corners);

            scalar_t* out = output_data + n * output_slice_size +
                od * output_height * output_width * channels +
                oh * output_width * channels + ow * channels;
            scalar_t* i000 = input_indexr(n, id0, ih0, iw0);
            scalar_t* i001 = input_indexr(n, id0, ih0, iw1);
            scalar_t* i010 = input_indexr(n, id0, ih1, iw0);
            scalar_t* i011 = input_indexr(n, id0, ih1, iw1);
            scalar_t* i100 = input_indexr(n, id1, ih0, iw0);
            scalar_t* i101 = input_indexr(n, id1, ih0, iw1);
            scalar_t* i110 = input_indexr(n, id1, ih1, iw0);
            scalar_t* i111 = input_indexr(n, id1, ih1, iw1);
            accscalar_t w000 = d0lambda * h0lambda * w0lambda;
            accscalar_t w001 = d0lambda * h0lambda * w1lambda;
            accscalar_t w010 = d0lambda * h1lambda * w0lambda;
            accscalar_t w011 = d0lambda * h1lambda * w1lambda;
            accscalar_t w100 = d1lambda * h0lambda * w0lambda;
            accscalar_t w101 = d1lambda * h0lambda * w1lambda;
            accscalar_t w110 = d1lambda * h1lambda * w0lambda;
            accscalar_t w111 = d1lambda * h1lambda * w1lambda;

            int64_t size = channels;
            int64_t d = 0;
            for (; d < size - (size % Vec::size()); d += Vec::size()) {
              auto out_vec = interpolate(
                  i000 + d,
                  w000,
                  i001 + d,
                  w001,
                  i010 + d,
                  w010,
                  i011 + d,
                  w011,
                  i100 + d,
                  w110,
                  i101 + d,
                  w101,
                  i110 + d,
                  w110,
                  i111 + d,
                  w111);
              store(out + d, out_vec);
            }
            for (; d < size; d++) {
              out[d] = i000[d] * w000 + i001[d] * w001 + i010[d] * w010 +
                  i011[d] * w011 + i100[d] * w100 + i101[d] * w101 +
                  i110[d] * w110 + i111[d] * w111;
            }
          }
        }
      }
    }
  };

  if (ndim == 4) {
    // upsample nearest 2d
    at::parallel_for(
        0,
        num_batches,
        at::internal::GRAIN_SIZE / output_slice_size / 4,
        loop2d);
  } else {
    // upsample nearest 3d
    TORCH_INTERNAL_ASSERT(ndim == 5);
    at::parallel_for(
        0,
        num_batches,
        at::internal::GRAIN_SIZE / output_slice_size / 8,
        loop3d);
  }

  if (!output_.is_contiguous(channels_last_memory_format)) {
    output_.copy_(output);
  }
}

// Helper structs to use with upsample_generic_Nd_kernel_body
struct HelperInterpBase {
  static inline void init_indices_weights(
      at::ScalarType output_type,
      std::vector<at::Tensor>& output,
      int64_t output_size,
      int64_t ndims,
      int64_t reshape_dim,
      int interp_size) {
    auto new_shape = std::vector<int64_t>(ndims, 1);
    new_shape[reshape_dim] = output_size;

    for (int j = 0; j < interp_size; j++) {
      output.emplace_back(
          at::empty(new_shape, at::CPU(c10::CppTypeToScalarType<int64_t>())));
      output.emplace_back(at::empty(new_shape, at::CPU(output_type)));
    }
  }
};

struct HelperInterpNearest : public HelperInterpBase {
  static const int interp_size = 1;

  static inline void init_indices_weights(
      at::ScalarType output_type,
      std::vector<at::Tensor>& output,
      int64_t output_size,
      int64_t ndims,
      int64_t reshape_dim,
      int interp_size) {
    auto new_shape = std::vector<int64_t>(ndims, 1);
    new_shape[reshape_dim] = output_size;

    for (int j = 0; j < interp_size; j++) {
      output.emplace_back(
          at::empty(new_shape, at::CPU(c10::CppTypeToScalarType<int64_t>())));
      // Defines weights for consistency, but not used
      output.emplace_back(at::ones(new_shape, at::CPU(output_type)));
    }
  }

  // Compute nearest mode indices and weights for each interpolated dimension
  // indices_weights = {
  //      {indices_0, 1.0, },  // dim -n
  //      {indices_0, 1.0, },  // dim -(n-1)
  //      ...
  //      {indices_0, 1.0, },  // dim -1
  // }
  // Indices and weights are reshaped as (1, 1, ..., N, ..., 1, 1) to
  // fit input/output tensors.
  // Indices are already containing the strides to optimize the computations
  static inline std::vector<at::Tensor> compute_indices_weights(
      at::ScalarType scalar_type,
      int64_t input_size,
      int64_t output_size,
      int64_t stride,
      int64_t ndims,
      int64_t reshape_dim,
      bool align_corners,
      const c10::optional<double> opt_scale) {
    std::vector<at::Tensor> output;
    HelperInterpNearest::init_indices_weights(
        scalar_type,
        output,
        output_size,
        ndims,
        reshape_dim,
        HelperInterpNearest::interp_size);

    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        scalar_type,
        "compute_indices_weights_nearest",
        [&] {
          scalar_t scale = at::native::area_pixel_compute_scale<scalar_t>(
              input_size, output_size, align_corners, opt_scale);

          auto input_index_ptr = output[0].data_ptr<int64_t>();
          int64_t input_index;

          for (int64_t i = 0; i < output_size; i++) {
            const scalar_t real_input_index =
                at::native::area_pixel_compute_source_index<scalar_t>(
                    scale, i, /*align_corners=*/true, /*cubic=*/false);
            input_index = static_cast<int64_t>(floorf(real_input_index));
            input_index_ptr[i] =
                static_cast<int64_t>(std::min(input_index, input_size - 1)) *
                stride;
          }
        });
    return output;
  }
};

struct HelperInterpLinear : public HelperInterpBase {
  static const int interp_size = 2;

  // Compute indices and weights for each interpolated dimension
  // indices_weights = {
  //      {indices_0, weights_0, indices_1, weights_1},  // dim -n
  //      {indices_0, weights_0, indices_1, weights_1},  // dim -(n-1)
  //      ...
  //      {indices_0, weights_0, indices_1, weights_1},  // dim -1
  // }
  // Indices and weights are reshaped as (1, 1, ..., N, ..., 1, 1) to
  // fit input/output tensors.
  // Indices are already containing the strides to optimize the computations
  static inline std::vector<at::Tensor> compute_indices_weights(
      at::ScalarType scalar_type,
      int64_t input_size,
      int64_t output_size,
      int64_t stride,
      int64_t ndims,
      int64_t reshape_dim,
      bool align_corners,
      const c10::optional<double> opt_scale) {
    std::vector<at::Tensor> output;
    HelperInterpLinear::init_indices_weights(
        scalar_type,
        output,
        output_size,
        ndims,
        reshape_dim,
        HelperInterpLinear::interp_size);

    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        scalar_type,
        "compute_indices_weights_linear",
        [&] {
          scalar_t scale = at::native::area_pixel_compute_scale<scalar_t>(
              input_size, output_size, align_corners, opt_scale);

          auto input_index0_ptr = output[0].data_ptr<int64_t>();
          auto lambda0_ptr = output[1].data_ptr<scalar_t>();
          auto input_index1_ptr = output[2].data_ptr<int64_t>();
          auto lambda1_ptr = output[3].data_ptr<scalar_t>();

          for (int64_t i = 0; i < output_size; i++) {
            at::native::compute_source_index_and_lambda<scalar_t>(
                input_index0_ptr[i],
                input_index1_ptr[i],
                lambda0_ptr[i],
                lambda1_ptr[i],
                scale,
                i,
                input_size,
                output_size,
                align_corners);
            // put stride into indices
            // index values correspond to input indices (0, 1, 2, 3, ...)
            // when multiplied by input stride, maximum possible value
            // input_size[dim-1] * input_size[dim-2] * ... for the given
            // dimension.
            input_index0_ptr[i] *= stride;
            input_index1_ptr[i] *= stride;
          }
        });
    return output;
  }
};

struct HelperInterpCubic : public HelperInterpBase {
  static const int interp_size = 4;

  // Compute indices and weights for each interpolated dimension
  // indices_weights = {
  //      {indices_0, weights_0, indices_1, weights_1, ..., indices_3,
  //      weights_3},  // dim -n {indices_0, weights_0, indices_1, weights_1,
  //      ..., indices_3, weights_3},  // dim -(n-1)
  //      ...
  //      {indices_0, weights_0, indices_1, weights_1, ..., indices_3,
  //      weights_3},  // dim -1
  // }
  // Indices and weights are reshaped as (1, 1, ..., N, ..., 1, 1) to
  // fit input/output tensors.
  // Indices are already containing the strides to optimize the computations
  static inline std::vector<at::Tensor> compute_indices_weights(
      at::ScalarType scalar_type,
      int64_t input_size,
      int64_t output_size,
      int64_t stride,
      int64_t ndims,
      int64_t reshape_dim,
      bool align_corners,
      const c10::optional<double> opt_scale) {
    std::vector<at::Tensor> output;
    HelperInterpCubic::init_indices_weights(
        scalar_type,
        output,
        output_size,
        ndims,
        reshape_dim,
        HelperInterpCubic::interp_size);

    AT_DISPATCH_FLOATING_TYPES(
        scalar_type, "compute_indices_weights_cubic", [&] {
          scalar_t scale = at::native::area_pixel_compute_scale<scalar_t>(
              input_size, output_size, align_corners, opt_scale);

          int64_t input_index;
          int64_t zero = static_cast<int64_t>(0);
          // NOLINTNEXTLINE(cppcoreguidelines-avoid-c-arrays,modernize-avoid-c-arrays)
          scalar_t coeffs[4];

          int64_t* idx_ptr;
          scalar_t* wt_ptr;

          for (int64_t i = 0; i < output_size; i++) {
            const scalar_t real_input_index =
                at::native::area_pixel_compute_source_index<scalar_t>(
                    scale, i, align_corners, /*cubic=*/true);
            input_index = static_cast<int64_t>(floorf(real_input_index));
            at::native::get_cubic_upsample_coefficients<scalar_t>(
                coeffs, real_input_index - input_index);

            for (int j = 0; j < interp_size; j++) {
              idx_ptr = output[2 * j + 0].data_ptr<int64_t>();
              idx_ptr[i] =
                  static_cast<int64_t>(std::max(
                      std::min(input_index + j - 1, input_size - 1), zero)) *
                  stride;
              wt_ptr = output[2 * j + 1].data_ptr<scalar_t>();
              wt_ptr[i] = coeffs[j];
            }
          }
        });
    return output;
  }
};

// Generic upsampling interpolation kernel for N-d case.
// Input is assumed to be like NCHW, NCL, NCKHW - interpolated spatial dimension
// are those from the end up to batch size N and number of channels C.
//
// Internally, it uses TensorIterator to optimize the computations.
// - out_ndims is the number of interpolated dims: 1, 2, 3
// - scale_type is template type for scales, typically c10::optional<double>
// - template<typename> class F is one of the above structs to compute indices
// and weights
template <int out_ndims, typename scale_type, class F>
void upsample_generic_Nd_kernel_body(
    const at::Tensor& output,
    const at::Tensor& input,
    bool align_corners,
    const scale_type& scales) {
  // input can be NCHW, NCL or NCKHW
  auto shape = input.sizes().vec();
  auto strides = input.strides().vec();
  auto oshape = output.sizes();

  TORCH_INTERNAL_ASSERT(
      shape.size() == oshape.size() && shape.size() == 2 + out_ndims);
  TORCH_INTERNAL_ASSERT(strides.size() == 2 + out_ndims);

  for (int i = 0; i < out_ndims; i++) {
    shape[i + 2] = oshape[i + 2];
    strides[i + 2] = 0;
  }
  auto restrided_input = input.as_strided(shape, strides);

  std::vector<std::vector<at::Tensor>> indices_weights;

  constexpr int interp_size = F::interp_size;
  auto input_scalar_type = input.scalar_type();
  if ((interp_size == 1 && input_scalar_type == at::ScalarType::Byte)) {
    // nearest also supports uint8 tensor, but we have to use float
    // with compute_indices_weights
    input_scalar_type = at::ScalarType::Float;
  }

  for (int i = 0; i < out_ndims; i++) {
    // NOLINTNEXTLINE(performance-inefficient-vector-operation)
    indices_weights.emplace_back(F::compute_indices_weights(
        input_scalar_type,
        input.size(i + 2),
        oshape[i + 2],
        input.stride(i + 2) * input.element_size(),
        input.dim(),
        i + 2,
        align_corners,
        scales[i]));
  }

  at::TensorIteratorConfig config;
  config.check_all_same_dtype(false)
      .declare_static_dtype_and_device(input.scalar_type(), input.device())
      .add_output(output)
      .add_input(restrided_input);

  for (auto& idx_weight : indices_weights) {
    for (auto& tensor : idx_weight) {
      config.add_input(tensor);
    }
  }

  auto iter = config.build();

  if (interp_size > 1) {
    // Nearest also supports uint8 tensor, so need to handle it separately
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16, iter.dtype(), "upsample_generic_Nd", [&] {
          // MSVC can not catch constexpr int interp_size here
          constexpr int mode = F::interp_size;
          cpu_upsample_generic<scalar_t, out_ndims, mode>(iter);
        });
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Byte,
        at::ScalarType::BFloat16,
        iter.dtype(),
        "upsample_generic_Nd",
        [&] {
          constexpr int mode = F::interp_size;
          cpu_upsample_generic<scalar_t, out_ndims, mode>(iter);
        });
  }
}

void upsample_nearest1d_kernel_impl(
    const at::Tensor& output,
    const at::Tensor& input,
    c10::optional<double> scales_w) {
  upsample_generic_Nd_kernel_body<1, scale_t, HelperInterpNearest>(
      output, input, false, {scales_w});
}

void upsample_nearest2d_kernel_impl(
    const at::Tensor& output,
    const at::Tensor& input,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  if (input.is_contiguous(at::MemoryFormat::ChannelsLast)) {
    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Byte,
        at::ScalarType::BFloat16,
        input.scalar_type(),
        "upsample_nearest2d_channels_last",
        [&] {
          cpu_upsample_nearest_channels_last<scalar_t, scale_t>(
              output, input, {scales_h, scales_w});
        });
  } else {
    upsample_generic_Nd_kernel_body<2, scale_t, HelperInterpNearest>(
        output, input, false, {scales_h, scales_w});
  }
}

void upsample_nearest3d_kernel_impl(
    const at::Tensor& output,
    const at::Tensor& input,
    c10::optional<double> scales_d,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  if (input.is_contiguous(at::MemoryFormat::ChannelsLast3d)) {
    AT_DISPATCH_FLOATING_TYPES_AND2(
        at::ScalarType::Byte,
        at::ScalarType::BFloat16,
        input.scalar_type(),
        "upsample_nearest3d_channels_last",
        [&] {
          cpu_upsample_nearest_channels_last<scalar_t, scale_t>(
              output, input, {scales_d, scales_h, scales_w});
        });
  } else {
    upsample_generic_Nd_kernel_body<3, scale_t, HelperInterpNearest>(
        output, input, false, {scales_d, scales_h, scales_w});
  }
}

void upsample_linear1d_kernel_impl(
    const at::Tensor& output,
    const at::Tensor& input,
    bool align_corners,
    c10::optional<double> scales_w) {
  upsample_generic_Nd_kernel_body<1, scale_t, HelperInterpLinear>(
      output, input, align_corners, {scales_w});
}

void upsample_bilinear2d_kernel_impl(
    const at::Tensor& output,
    const at::Tensor& input,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  // Temporarily dispatch to original channels last implementation
  if (input.is_contiguous(at::MemoryFormat::ChannelsLast)) {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        input.scalar_type(),
        "upsample_bilinear2d_channels_last",
        [&] {
          cpu_upsample_linear_channels_last<scalar_t, scale_t>(
              output, input, align_corners, {scales_h, scales_w});
        });
  } else {
    upsample_generic_Nd_kernel_body<2, scale_t, HelperInterpLinear>(
        output, input, align_corners, {scales_h, scales_w});
  }
}

void upsample_trilinear3d_kernel_impl(
    const at::Tensor& output,
    const at::Tensor& input,
    bool align_corners,
    c10::optional<double> scales_d,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  if (input.is_contiguous(at::MemoryFormat::ChannelsLast3d)) {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        input.scalar_type(),
        "upsample_trilinear3d_channels_last",
        [&] {
          cpu_upsample_linear_channels_last<scalar_t, scale_t>(
              output, input, align_corners, {scales_d, scales_h, scales_w});
        });
  } else {
    upsample_generic_Nd_kernel_body<3, scale_t, HelperInterpLinear>(
        output, input, align_corners, {scales_d, scales_h, scales_w});
  }
}

void upsample_bicubic2d_kernel_impl(
    const at::Tensor& output,
    const at::Tensor& input,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  upsample_generic_Nd_kernel_body<2, scale_t, HelperInterpCubic>(
      output, input, align_corners, {scales_h, scales_w});
}

template <typename scalar_t, typename scale_type>
void cpu_upsample_nearest_backward(
    const at::Tensor& grad_input_,
    const at::Tensor& grad_output_,
    const scale_type& scales) {
  TORCH_CHECK(
      grad_input_.dtype() == grad_output_.dtype(),
      "expected dtype ",
      grad_output_.dtype(),
      " for `grad_input` but got dtype ",
      grad_input_.dtype());

  auto grad_output = grad_output_.contiguous();
  auto grad_input = grad_input_.contiguous();

  auto grad_output_data = grad_output.data_ptr<scalar_t>();
  auto grad_input_data = grad_input.data_ptr<scalar_t>();
  auto input_sizes = grad_input.sizes().vec();
  auto output_sizes = grad_output.sizes().vec();
  auto ndim = input_sizes.size();

  // treat nbatch and channels as one dimension
  int64_t channels = input_sizes[0] * input_sizes[1];
  int64_t input_depth = (ndim == 5) ? input_sizes[2] : 1;
  int64_t output_depth = (ndim == 5) ? output_sizes[2] : 1;
  int64_t input_height = (ndim >= 4) ? input_sizes[ndim - 2] : 1;
  int64_t output_height = (ndim >= 4) ? output_sizes[ndim - 2] : 1;
  int64_t input_width = input_sizes[ndim - 1];
  int64_t output_width = output_sizes[ndim - 1];

  int64_t output_slice_size = output_depth * output_height * output_width;
  int64_t input_slice_size = input_depth * input_height * input_width;

  auto loop1d = [&](int64_t begin, int64_t end) {
    for (int64_t c = begin; c < end; c++) {
      for (int64_t ow = 0; ow < output_width; ow++) {
        int64_t iw = nearest_idx(ow, input_width, output_width, scales[0]);
        int64_t output_offset = c * output_slice_size + ow;
        int64_t input_offset = c * input_slice_size + iw;
        grad_input_data[input_offset] += grad_output_data[output_offset];
      }
    }
  };

  auto loop2d = [&](int64_t begin, int64_t end) {
    for (int64_t c = begin; c < end; c++) {
      for (int64_t oh = 0; oh < output_height; oh++) {
        int64_t ih = nearest_idx(oh, input_height, output_height, scales[0]);
        for (int64_t ow = 0; ow < output_width; ow++) {
          int64_t iw = nearest_idx(ow, input_width, output_width, scales[1]);
          int64_t output_offset =
              c * output_slice_size + oh * output_width + ow;
          int64_t input_offset = c * input_slice_size + ih * input_width + iw;
          grad_input_data[input_offset] += grad_output_data[output_offset];
        }
      }
    }
  };

  auto loop3d = [&](int64_t begin, int64_t end) {
    for (int64_t c = begin; c < end; c++) {
      for (int64_t od = 0; od < output_depth; od++) {
        int64_t id = nearest_idx(od, input_depth, output_depth, scales[0]);
        for (int64_t oh = 0; oh < output_height; oh++) {
          int64_t ih = nearest_idx(oh, input_height, output_height, scales[1]);
          for (int64_t ow = 0; ow < output_width; ow++) {
            int64_t iw = nearest_idx(ow, input_width, output_width, scales[2]);
            int64_t output_offset = c * output_slice_size +
                od * output_height * output_width + oh * output_width + ow;
            int64_t input_offset = c * input_slice_size +
                id * input_height * input_width + ih * input_width + iw;
            grad_input_data[input_offset] += grad_output_data[output_offset];
          }
        }
      }
    }
  };

  if (ndim == 3) {
    // upsample nearest 1d
    at::parallel_for(
        0, channels, at::internal::GRAIN_SIZE / output_slice_size, loop1d);
  } else if (ndim == 4) {
    // upsample nearest 2d
    at::parallel_for(
        0, channels, at::internal::GRAIN_SIZE / output_slice_size, loop2d);
  } else {
    // upsample nearest 3d
    TORCH_INTERNAL_ASSERT(ndim == 5);
    at::parallel_for(
        0, channels, at::internal::GRAIN_SIZE / output_slice_size, loop3d);
  }

  if (!grad_input_.is_contiguous()) {
    grad_input_.copy_(grad_input);
  }
}

template <typename scalar_t, typename scale_type>
void cpu_upsample_nearest_backward_channels_last(
    const at::Tensor& grad_input_,
    const at::Tensor& grad_output_,
    const scale_type& scales) {
  TORCH_CHECK(
      grad_input_.dtype() == grad_output_.dtype(),
      "expected dtype ",
      grad_output_.dtype(),
      " for `grad_input` but got dtype ",
      grad_input_.dtype());

  auto ndim = grad_output_.ndimension();
  TORCH_CHECK(
      ndim >= 4 && ndim <= 5,
      "Upsample with NHWC format supports tensors with 4 or 5 dims.")

  auto channels_last_memory_format = ndim == 4
      ? at::MemoryFormat::ChannelsLast
      : at::MemoryFormat::ChannelsLast3d;
  auto grad_output = grad_output_.contiguous(channels_last_memory_format);
  auto grad_input = grad_input_.contiguous(channels_last_memory_format);

  auto grad_output_data = grad_output.data_ptr<scalar_t>();
  auto grad_input_data = grad_input.data_ptr<scalar_t>();

  auto input_sizes = grad_input.sizes().vec();
  auto output_sizes = grad_output.sizes().vec();

  int64_t num_batches = input_sizes[0];
  int64_t channels = input_sizes[1];
  int64_t input_depth = (ndim == 5) ? input_sizes[2] : 1;
  int64_t output_depth = (ndim == 5) ? output_sizes[2] : 1;
  int64_t input_height = (ndim >= 4) ? input_sizes[ndim - 2] : 1;
  int64_t output_height = (ndim >= 4) ? output_sizes[ndim - 2] : 1;
  int64_t input_width = input_sizes[ndim - 1];
  int64_t output_width = output_sizes[ndim - 1];

  using Vec = at::vec::Vectorized<scalar_t>;
  auto acc = [](scalar_t* gin, scalar_t* gout, int64_t size) {
    int64_t d = 0;
    for (; d < size - (size % Vec::size()); d += Vec::size()) {
      Vec gin_vec = Vec::loadu(gin + d) + Vec::loadu(gout + d);
      gin_vec.store(gin + d);
    }
    for (; d < size; d++) {
      gin[d] += gout[d];
    }
  };

  auto loop2d = [&](int64_t begin, int64_t end) {
    for (const auto n : c10::irange(begin, end)) {
      for (const auto oh : c10::irange(output_height)) {
        int64_t ih = nearest_idx(oh, input_height, output_height, scales[0]);
        for (const auto ow : c10::irange(output_width)) {
          int64_t iw = nearest_idx(ow, input_width, output_width, scales[1]);
          scalar_t* grad_output_ptr = grad_output_data +
              (n * output_height * output_width + oh * output_width + ow) *
                  channels;
          scalar_t* grad_input_ptr = grad_input_data +
              (n * input_height * input_width + ih * input_width + iw) *
                  channels;
          acc(grad_input_ptr, grad_output_ptr, channels);
        }
      }
    }
  };

  auto loop3d = [&](int64_t begin, int64_t end) {
    for (const auto n : c10::irange(begin, end)) {
      for (int64_t od = 0; od < output_depth; od++) {
        int64_t id = nearest_idx(od, input_depth, output_depth, scales[0]);
        for (int64_t oh = 0; oh < output_height; oh++) {
          int64_t ih = nearest_idx(oh, input_height, output_height, scales[1]);
          for (int64_t ow = 0; ow < output_width; ow++) {
            int64_t iw = nearest_idx(ow, input_width, output_width, scales[2]);
            scalar_t* grad_output_ptr = grad_output_data +
                (n * output_depth * output_height * output_width +
                 od * output_height * output_width + oh * output_width + ow) *
                    channels;
            scalar_t* grad_input_ptr = grad_input_data +
                (n * input_depth * input_height * input_width +
                 id * input_height * input_width + ih * input_width + iw) *
                    channels;
            acc(grad_input_ptr, grad_output_ptr, channels);
          }
        }
      }
    }
  };

  if (ndim == 4) {
    // upsample nearest 2d
    at::parallel_for(0, num_batches, 0, loop2d);
  } else {
    // upsample nearest 3d
    TORCH_INTERNAL_ASSERT(ndim == 5);
    at::parallel_for(0, num_batches, 0, loop3d);
  }

  if (!grad_input_.is_contiguous(channels_last_memory_format)) {
    grad_input_.copy_(grad_input);
  }
}

void upsample_nearest1d_backward_kernel_impl(
    const at::Tensor& grad_input,
    const at::Tensor& grad_output,
    c10::optional<double> scales_w) {
  AT_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      grad_output.scalar_type(),
      "upsample_nearest1d_backward",
      [&] {
        cpu_upsample_nearest_backward<scalar_t, scale_t>(
            grad_input, grad_output, {scales_w});
      });
}

void upsample_nearest2d_backward_kernel_impl(
    const at::Tensor& grad_input,
    const at::Tensor& grad_output,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  if (grad_output.is_contiguous(at::MemoryFormat::ChannelsLast)) {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_nearest2d_backward_channels_last",
        [&] {
          cpu_upsample_nearest_backward_channels_last<scalar_t, scale_t>(
              grad_input, grad_output, {scales_h, scales_w});
        });
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_nearest2d_backward",
        [&] {
          cpu_upsample_nearest_backward<scalar_t, scale_t>(
              grad_input, grad_output, {scales_h, scales_w});
        });
  }
}

void upsample_nearest3d_backward_kernel_impl(
    const at::Tensor& grad_input,
    const at::Tensor& grad_output,
    c10::optional<double> scales_d,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  if (grad_output.is_contiguous(at::MemoryFormat::ChannelsLast3d)) {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_nearest3d_backward_channels_last",
        [&] {
          cpu_upsample_nearest_backward_channels_last<scalar_t, scale_t>(
              grad_input, grad_output, {scales_d, scales_h, scales_w});
        });
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_nearest3d_backward",
        [&] {
          cpu_upsample_nearest_backward<scalar_t, scale_t>(
              grad_input, grad_output, {scales_d, scales_h, scales_w});
        });
  }
}

template <typename scalar_t, typename scale_type>
void cpu_upsample_linear_backward(
    const at::Tensor& grad_input_,
    const at::Tensor& grad_output_,
    bool align_corners,
    const scale_type& scales) {
  TORCH_CHECK(
      grad_input_.dtype() == grad_output_.dtype(),
      "expected dtype ",
      grad_output_.dtype(),
      " for `grad_input` but got dtype ",
      grad_input_.dtype());

  auto grad_output = grad_output_.contiguous();
  auto grad_input = grad_input_.contiguous();

  auto grad_output_data = grad_output.data_ptr<scalar_t>();
  auto grad_input_data = grad_input.data_ptr<scalar_t>();
  auto input_sizes = grad_input.sizes().vec();
  auto output_sizes = grad_output.sizes().vec();
  auto ndim = input_sizes.size();

  // treat nbatch and channels as one dimension
  int64_t channels = input_sizes[0] * input_sizes[1];
  int64_t input_depth = (ndim == 5) ? input_sizes[2] : 1;
  int64_t output_depth = (ndim == 5) ? output_sizes[2] : 1;
  int64_t input_height = (ndim >= 4) ? input_sizes[ndim - 2] : 1;
  int64_t output_height = (ndim >= 4) ? output_sizes[ndim - 2] : 1;
  int64_t input_width = input_sizes[ndim - 1];
  int64_t output_width = output_sizes[ndim - 1];

  int64_t output_slice_size = output_depth * output_height * output_width;

  auto loop1d = [&](int64_t begin, int64_t end) {
    const scalar_t width_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_width, output_width, align_corners, scales[0]);

    auto input_indexr = [=](int64_t c, int64_t w) {
      return grad_input_data + c * input_width + w;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int64_t iw0, iw1;
    scalar_t w0lambda, w1lambda;
    for (int64_t c = begin; c < end; c++) {
      for (int64_t ow = 0; ow < output_width; ow++) {
        at::native::compute_source_index_and_lambda(
            iw0,
            iw1,
            w0lambda,
            w1lambda,
            width_scale,
            ow,
            input_width,
            output_width,
            align_corners);
        scalar_t grad_output_value =
            grad_output_data[c * output_slice_size + ow];
        *input_indexr(c, iw0) += w0lambda * grad_output_value; /* i0 */
        *input_indexr(c, iw1) += w1lambda * grad_output_value; /* i1*/
      }
    }
  };

  auto loop2d = [&](int64_t begin, int64_t end) {
    const scalar_t height_scale =
        at::native::area_pixel_compute_scale<scalar_t>(
            input_height, output_height, align_corners, scales[0]);
    const scalar_t width_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_width, output_width, align_corners, scales[1]);

    auto input_indexr = [=](int64_t c, int64_t h, int64_t w) {
      return grad_input_data + c * input_height * input_width +
          h * input_width + w;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int64_t ih0, ih1, iw0, iw1;
    scalar_t h0lambda, h1lambda, w0lambda, w1lambda;
    for (int64_t c = begin; c < end; c++) {
      for (int64_t oh = 0; oh < output_height; oh++) {
        at::native::compute_source_index_and_lambda(
            ih0,
            ih1,
            h0lambda,
            h1lambda,
            height_scale,
            oh,
            input_height,
            output_height,
            align_corners);
        for (int64_t ow = 0; ow < output_width; ow++) {
          at::native::compute_source_index_and_lambda(
              iw0,
              iw1,
              w0lambda,
              w1lambda,
              width_scale,
              ow,
              input_width,
              output_width,
              align_corners);
          scalar_t grad_output_value =
              grad_output_data[c * output_slice_size + oh * output_width + ow];
          *input_indexr(c, ih0, iw0) +=
              h0lambda * w0lambda * grad_output_value; /* i00 */
          *input_indexr(c, ih0, iw1) +=
              h0lambda * w1lambda * grad_output_value; /* i01 */
          *input_indexr(c, ih1, iw0) +=
              h1lambda * w0lambda * grad_output_value; /* i10 */
          *input_indexr(c, ih1, iw1) +=
              h1lambda * w1lambda * grad_output_value; /* i11 */
        }
      }
    }
  };

  auto loop3d = [&](int64_t begin, int64_t end) {
    const scalar_t depth_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_depth, output_depth, align_corners, scales[0]);
    const scalar_t height_scale =
        at::native::area_pixel_compute_scale<scalar_t>(
            input_height, output_height, align_corners, scales[1]);
    const scalar_t width_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_width, output_width, align_corners, scales[2]);

    auto input_indexr = [=](int64_t c, int64_t d, int64_t h, int64_t w) {
      return grad_input_data + c * input_depth * input_height * input_width +
          d * input_height * input_width + h * input_width + w;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int64_t id0, id1, ih0, ih1, iw0, iw1;
    scalar_t d0lambda, d1lambda, h0lambda, h1lambda, w0lambda, w1lambda;
    for (int64_t c = begin; c < end; c++) {
      for (int64_t od = 0; od < output_depth; od++) {
        at::native::compute_source_index_and_lambda(
            id0,
            id1,
            d0lambda,
            d1lambda,
            depth_scale,
            od,
            input_depth,
            output_depth,
            align_corners);
        for (int64_t oh = 0; oh < output_height; oh++) {
          at::native::compute_source_index_and_lambda(
              ih0,
              ih1,
              h0lambda,
              h1lambda,
              height_scale,
              oh,
              input_height,
              output_height,
              align_corners);
          for (int64_t ow = 0; ow < output_width; ow++) {
            at::native::compute_source_index_and_lambda(
                iw0,
                iw1,
                w0lambda,
                w1lambda,
                width_scale,
                ow,
                input_width,
                output_width,
                align_corners);
            scalar_t grad_output_value = grad_output_data
                [c * output_slice_size + od * output_height * output_width +
                 oh * output_width + ow];
            *input_indexr(c, id0, ih0, iw0) +=
                d0lambda * h0lambda * w0lambda * grad_output_value; /* i000 */
            *input_indexr(c, id0, ih0, iw1) +=
                d0lambda * h0lambda * w1lambda * grad_output_value; /* i001 */
            *input_indexr(c, id0, ih1, iw0) +=
                d0lambda * h1lambda * w0lambda * grad_output_value; /* i010 */
            *input_indexr(c, id0, ih1, iw1) +=
                d0lambda * h1lambda * w1lambda * grad_output_value; /* i011 */
            *input_indexr(c, id1, ih0, iw0) +=
                d1lambda * h0lambda * w0lambda * grad_output_value; /* i100 */
            *input_indexr(c, id1, ih0, iw1) +=
                d1lambda * h0lambda * w1lambda * grad_output_value; /* i101 */
            *input_indexr(c, id1, ih1, iw0) +=
                d1lambda * h1lambda * w0lambda * grad_output_value; /* i110 */
            *input_indexr(c, id1, ih1, iw1) +=
                d1lambda * h1lambda * w1lambda * grad_output_value; /* i111 */
          }
        }
      }
    }
  };

  if (ndim == 3) {
    // upsample linear 1d
    at::parallel_for(
        0, channels, at::internal::GRAIN_SIZE / output_slice_size / 2, loop1d);
  } else if (ndim == 4) {
    // upsample bilinear 2d
    at::parallel_for(
        0, channels, at::internal::GRAIN_SIZE / output_slice_size / 4, loop2d);
  } else {
    // upsample trilinear 3d
    TORCH_INTERNAL_ASSERT(ndim == 5);
    at::parallel_for(
        0, channels, at::internal::GRAIN_SIZE / output_slice_size / 8, loop3d);
  }

  if (!grad_input_.is_contiguous()) {
    grad_input_.copy_(grad_input);
  }
}

template <typename scalar_t, typename scale_type>
void cpu_upsample_linear_backward_channels_last(
    const at::Tensor& grad_input_,
    const at::Tensor& grad_output_,
    bool align_corners,
    const scale_type& scales) {
  TORCH_CHECK(
      grad_input_.dtype() == grad_output_.dtype(),
      "expected dtype ",
      grad_output_.dtype(),
      " for `grad_input` but got dtype ",
      grad_input_.dtype());

  auto ndim = grad_output_.ndimension();
  TORCH_CHECK(
      ndim >= 4 && ndim <= 5,
      "Upsample with NHWC format supports tensors with 4 or 5 dims.")

  auto channels_last_memory_format = ndim == 4
      ? at::MemoryFormat::ChannelsLast
      : at::MemoryFormat::ChannelsLast3d;
  auto grad_output = grad_output_.contiguous(channels_last_memory_format);
  auto grad_input = grad_input_.contiguous(channels_last_memory_format);

  auto grad_output_data = grad_output.data_ptr<scalar_t>();
  auto grad_input_data = grad_input.data_ptr<scalar_t>();

  auto input_sizes = grad_input.sizes().vec();
  auto output_sizes = grad_output.sizes().vec();

  int64_t num_batches = input_sizes[0];
  int64_t channels = input_sizes[1];
  int64_t input_depth = (ndim == 5) ? input_sizes[2] : 1;
  int64_t output_depth = (ndim == 5) ? output_sizes[2] : 1;
  int64_t input_height = (ndim >= 4) ? input_sizes[ndim - 2] : 1;
  int64_t output_height = (ndim >= 4) ? output_sizes[ndim - 2] : 1;
  int64_t input_width = input_sizes[ndim - 1];
  int64_t output_width = output_sizes[ndim - 1];

  using accscalar_t = at::acc_type<scalar_t, false>;
  using Vec = at::vec::Vectorized<scalar_t>;
  auto acc = [](scalar_t* gin, scalar_t* gout, accscalar_t w, int64_t size) {
    int64_t d = 0;
    for (; d < size - (size % Vec::size()); d += Vec::size()) {
      Vec gin_vec = Vec::loadu(gin + d) + Vec(w) * Vec::loadu(gout + d);
      gin_vec.store(gin + d);
    }
    for (; d < size; d++) {
      gin[d] += w * gout[d];
    }
  };

  auto loop2d = [&](int64_t begin, int64_t end) {
    const scalar_t height_scale =
        at::native::area_pixel_compute_scale<scalar_t>(
            input_height, output_height, align_corners, scales[0]);
    const scalar_t width_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_width, output_width, align_corners, scales[1]);

    auto input_indexr = [=](int64_t n, int64_t h, int64_t w) {
      return grad_input_data +
          (n * input_height * input_width + h * input_width + w) * channels;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int64_t ih0, ih1, iw0, iw1;
    scalar_t h0lambda, h1lambda, w0lambda, w1lambda;
    for (const auto n : c10::irange(begin, end)) {
      for (const auto oh : c10::irange(output_height)) {
        at::native::compute_source_index_and_lambda(
            ih0,
            ih1,
            h0lambda,
            h1lambda,
            height_scale,
            oh,
            input_height,
            output_height,
            align_corners);
        for (const auto ow : c10::irange(output_width)) {
          at::native::compute_source_index_and_lambda(
              iw0,
              iw1,
              w0lambda,
              w1lambda,
              width_scale,
              ow,
              input_width,
              output_width,
              align_corners);
          scalar_t* grad_output_ptr = grad_output_data +
              (n * output_height * output_width + oh * output_width + ow) *
                  channels;
          acc(input_indexr(n, ih0, iw0),
              grad_output_ptr,
              h0lambda * w0lambda,
              channels); /* i00 */
          acc(input_indexr(n, ih0, iw1),
              grad_output_ptr,
              h0lambda * w1lambda,
              channels); /* i01 */
          acc(input_indexr(n, ih1, iw0),
              grad_output_ptr,
              h1lambda * w0lambda,
              channels); /* i10 */
          acc(input_indexr(n, ih1, iw1),
              grad_output_ptr,
              h1lambda * w1lambda,
              channels); /* i11 */
        }
      }
    }
  };

  auto loop3d = [&](int64_t begin, int64_t end) {
    const scalar_t depth_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_depth, output_depth, align_corners, scales[0]);
    const scalar_t height_scale =
        at::native::area_pixel_compute_scale<scalar_t>(
            input_height, output_height, align_corners, scales[1]);
    const scalar_t width_scale = at::native::area_pixel_compute_scale<scalar_t>(
        input_width, output_width, align_corners, scales[2]);

    auto input_indexr = [=](int64_t n, int64_t d, int64_t h, int64_t w) {
      return grad_input_data +
          (n * input_depth * input_height * input_width +
           d * input_height * input_width + h * input_width + w) *
          channels;
    };

    // NOLINTNEXTLINE(cppcoreguidelines-init-variables)
    int64_t id0, id1, ih0, ih1, iw0, iw1;
    scalar_t d0lambda, d1lambda, h0lambda, h1lambda, w0lambda, w1lambda;
    for (const auto n : c10::irange(begin, end)) {
      for (const auto od : c10::irange(output_depth)) {
        at::native::compute_source_index_and_lambda(
            id0,
            id1,
            d0lambda,
            d1lambda,
            depth_scale,
            od,
            input_depth,
            output_depth,
            align_corners);
        for (const auto oh : c10::irange(output_height)) {
          at::native::compute_source_index_and_lambda(
              ih0,
              ih1,
              h0lambda,
              h1lambda,
              height_scale,
              oh,
              input_height,
              output_height,
              align_corners);
          for (const auto ow : c10::irange(output_width)) {
            at::native::compute_source_index_and_lambda(
                iw0,
                iw1,
                w0lambda,
                w1lambda,
                width_scale,
                ow,
                input_width,
                output_width,
                align_corners);
            scalar_t* grad_output_ptr = grad_output_data +
                (n * output_depth * output_height * output_width +
                 od * output_height * output_width + oh * output_width + ow) *
                    channels;
            acc(input_indexr(n, id0, ih0, iw0),
                grad_output_ptr,
                d0lambda * h0lambda * w0lambda,
                channels); /* i000 */
            acc(input_indexr(n, id0, ih0, iw1),
                grad_output_ptr,
                d0lambda * h0lambda * w1lambda,
                channels); /* i001 */
            acc(input_indexr(n, id0, ih1, iw0),
                grad_output_ptr,
                d0lambda * h1lambda * w0lambda,
                channels); /* i010 */
            acc(input_indexr(n, id0, ih1, iw1),
                grad_output_ptr,
                d0lambda * h1lambda * w1lambda,
                channels); /* i011 */
            acc(input_indexr(n, id1, ih0, iw0),
                grad_output_ptr,
                d1lambda * h0lambda * w0lambda,
                channels); /* i100 */
            acc(input_indexr(n, id1, ih0, iw1),
                grad_output_ptr,
                d1lambda * h0lambda * w1lambda,
                channels); /* i101 */
            acc(input_indexr(n, id1, ih1, iw0),
                grad_output_ptr,
                d1lambda * h1lambda * w0lambda,
                channels); /* i110 */
            acc(input_indexr(n, id1, ih1, iw1),
                grad_output_ptr,
                d1lambda * h1lambda * w1lambda,
                channels); /* i111 */
          }
        }
      }
    }
  };

  if (ndim == 4) {
    // upsample bilinear 2d
    at::parallel_for(0, num_batches, 0, loop2d);
  } else {
    // upsample trilinear 3d
    TORCH_INTERNAL_ASSERT(ndim == 5);
    at::parallel_for(0, num_batches, 0, loop3d);
  }

  if (!grad_input_.is_contiguous(channels_last_memory_format)) {
    grad_input_.copy_(grad_input);
  }
}

void upsample_linear1d_backward_kernel_impl(
    const at::Tensor& grad_input,
    const at::Tensor& grad_output,
    bool align_corners,
    c10::optional<double> scales_w) {
  AT_DISPATCH_FLOATING_TYPES_AND(
      at::ScalarType::BFloat16,
      grad_output.scalar_type(),
      "upsample_linear1d_backward",
      [&] {
        cpu_upsample_linear_backward<scalar_t, scale_t>(
            grad_input, grad_output, align_corners, {scales_w});
      });
}

void upsample_bilinear2d_backward_kernel_impl(
    const at::Tensor& grad_input,
    const at::Tensor& grad_output,
    bool align_corners,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  if (grad_output.is_contiguous(at::MemoryFormat::ChannelsLast)) {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_bilinear2d_backward_channels_last",
        [&] {
          cpu_upsample_linear_backward_channels_last<scalar_t, scale_t>(
              grad_input, grad_output, align_corners, {scales_h, scales_w});
        });
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_bilinear2d_backward",
        [&] {
          cpu_upsample_linear_backward<scalar_t, scale_t>(
              grad_input, grad_output, align_corners, {scales_h, scales_w});
        });
  }
}

void upsample_trilinear3d_backward_kernel_impl(
    const at::Tensor& grad_input,
    const at::Tensor& grad_output,
    bool align_corners,
    c10::optional<double> scales_d,
    c10::optional<double> scales_h,
    c10::optional<double> scales_w) {
  if (grad_output.is_contiguous(at::MemoryFormat::ChannelsLast3d)) {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_trilinear3d_backward_channels_last",
        [&] {
          cpu_upsample_linear_backward_channels_last<scalar_t, scale_t>(
              grad_input,
              grad_output,
              align_corners,
              {scales_d, scales_h, scales_w});
        });
  } else {
    AT_DISPATCH_FLOATING_TYPES_AND(
        at::ScalarType::BFloat16,
        grad_output.scalar_type(),
        "upsample_trilinear3d_backward",
        [&] {
          cpu_upsample_linear_backward<scalar_t, scale_t>(
              grad_input,
              grad_output,
              align_corners,
              {scales_d, scales_h, scales_w});
        });
  }
}

} // anonymous namespace

REGISTER_DISPATCH(
    upsample_nearest1d_kernel_stub,
    &upsample_nearest1d_kernel_impl);

REGISTER_DISPATCH(
    upsample_nearest2d_kernel_stub,
    &upsample_nearest2d_kernel_impl);

REGISTER_DISPATCH(
    upsample_nearest3d_kernel_stub,
    &upsample_nearest3d_kernel_impl);

REGISTER_DISPATCH(
    upsample_linear1d_kernel_stub,
    &upsample_linear1d_kernel_impl);

REGISTER_DISPATCH(
    upsample_bilinear2d_kernel_stub,
    &upsample_bilinear2d_kernel_impl);

REGISTER_DISPATCH(
    upsample_trilinear3d_kernel_stub,
    &upsample_trilinear3d_kernel_impl);

REGISTER_DISPATCH(
    upsample_bicubic2d_kernel_stub,
    &upsample_bicubic2d_kernel_impl);

REGISTER_DISPATCH(
    upsample_nearest1d_backward_kernel_stub,
    &upsample_nearest1d_backward_kernel_impl);

REGISTER_DISPATCH(
    upsample_nearest2d_backward_kernel_stub,
    &upsample_nearest2d_backward_kernel_impl);

REGISTER_DISPATCH(
    upsample_nearest3d_backward_kernel_stub,
    &upsample_nearest3d_backward_kernel_impl);

REGISTER_DISPATCH(
    upsample_linear1d_backward_kernel_stub,
    &upsample_linear1d_backward_kernel_impl);

REGISTER_DISPATCH(
    upsample_bilinear2d_backward_kernel_stub,
    &upsample_bilinear2d_backward_kernel_impl);

REGISTER_DISPATCH(
    upsample_trilinear3d_backward_kernel_stub,
    &upsample_trilinear3d_backward_kernel_impl);

} // namespace cpu
} // namespace torch_ipex
