/* Copyright 2017 The TensorFlow Authors. All Rights Reserved.

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
#include <stdlib.h>
#include <string.h>

#include "tensorflow/contrib/lite/builtin_op_data.h"
#include "tensorflow/contrib/lite/kernels/activation_functor.h"
#include "tensorflow/contrib/lite/kernels/internal/common.h"
#include "tensorflow/contrib/lite/kernels/internal/compatibility.h"
#include "tensorflow/contrib/lite/kernels/internal/optimized/tensor_utils_impl.h"
#include "tensorflow/contrib/lite/kernels/internal/round.h"

#ifdef USE_NEON

#define kFloatWeightsPerNeonLane 4

namespace tflite {
namespace tensor_utils {
namespace {

// Allocates, at least, size bytes of uninitialized storage whose alignment is
// specified by alignment. The size parameter must be an integral multiple of
// alignment.
// Caller is responsible by freeing the allocated memory by calling free on
// the passed freeing_buffer pointer.
void* aligned_alloc(size_t alignment, size_t size, void** freeing_buffer) {
  *freeing_buffer = malloc(size + alignment);
  const size_t offset = ((uintptr_t)*freeing_buffer) % alignment;  // NOLINT
  return offset == 0
             ? *freeing_buffer
             : ((char*)*freeing_buffer + (alignment - offset));  // NOLINT
}

}  // namespace

void NeonMatrixBatchVectorMultiplyAccumulate(const float* matrix, int m_rows,
                                             int m_cols, const float* vector,
                                             int n_batch, float* result,
                                             int result_stride) {
  // If v_size is not divisible by kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start =
      m_cols - (m_cols & (kFloatWeightsPerNeonLane - 1));

  // The arrays used to cache the vector.
  void* aligned_vector_cache_free = nullptr;
  float32x4_t* vector_cache_float32x4 =
      reinterpret_cast<float32x4_t*>(aligned_alloc(
          sizeof(float32x4_t), (postamble_start >> 2) * sizeof(float32x4_t),
          &aligned_vector_cache_free));

  const int kUnrollSize = 2;
  for (int b = 0; b < n_batch; b++) {
    float* result_in_batch = result + b * m_rows * result_stride;
    const float* vector_in_batch = vector + b * m_cols;

    const float* matrix_ptr0 = matrix;
    // If there is only 1 row, we don't want to assign an illegal pointer.
    const float* matrix_ptr1 = nullptr;
    if (m_rows > 1) {
      matrix_ptr1 = matrix + m_cols;
    }

    // Cache the vector.
    for (int c = 0; c < postamble_start; c += kFloatWeightsPerNeonLane) {
      vector_cache_float32x4[c >> 2] = vld1q_f32(vector_in_batch + c);
    }

    // Main matrix by vector multiplication loop, which handles two rows of
    // matrix by vector multiplication.
    for (int r = 0; r < (m_rows & ~(kUnrollSize - 1)); r += kUnrollSize) {
      float32x4_t acc0_32x4 = vmovq_n_f32(0.0);
      float32x4_t acc1_32x4 = vmovq_n_f32(0.0);
      for (int c = 0; c < postamble_start; c += kFloatWeightsPerNeonLane) {
        float32x4_t temp = vector_cache_float32x4[c >> 2];
        // Load 4 float values from vector1 and vector2 and accumulator.
        float32x4_t v0_f32x4 = vld1q_f32(matrix_ptr0 + c);
        float32x4_t v1_f32x4 = vld1q_f32(matrix_ptr1 + c);
        // Vector multiply-accumulate 4 float
        acc0_32x4 = vmlaq_f32(acc0_32x4, v0_f32x4, temp);
        acc1_32x4 = vmlaq_f32(acc1_32x4, v1_f32x4, temp);
      }
      // Add the 4 intermediate sum values to get the final dot-prod value for
      // this column.
      *result_in_batch +=
          (vgetq_lane_f32(acc0_32x4, 0) + vgetq_lane_f32(acc0_32x4, 1) +
           vgetq_lane_f32(acc0_32x4, 2) + vgetq_lane_f32(acc0_32x4, 3));
      *(result_in_batch + result_stride) +=
          (vgetq_lane_f32(acc1_32x4, 0) + vgetq_lane_f32(acc1_32x4, 1) +
           vgetq_lane_f32(acc1_32x4, 2) + vgetq_lane_f32(acc1_32x4, 3));
      for (int c = postamble_start; c < m_cols; c++) {
        *result_in_batch += matrix_ptr0[c] * vector_in_batch[c];
        *(result_in_batch + result_stride) +=
            matrix_ptr1[c] * vector_in_batch[c];
      }
      matrix_ptr0 += kUnrollSize * m_cols;
      matrix_ptr1 += kUnrollSize * m_cols;
      result_in_batch += kUnrollSize * result_stride;
    }
    for (int r = (m_rows & ~(kUnrollSize - 1)); r < m_rows; r++) {
      float32x4_t acc0_32x4 = vmovq_n_f32(0.0);
      for (int c = 0; c < postamble_start; c += kFloatWeightsPerNeonLane) {
        float32x4_t temp = vector_cache_float32x4[c >> 2];
        // Load 4 float values from vector1 and vector2 and accumulator.
        float32x4_t v0_f32x4 = vld1q_f32(matrix_ptr0 + c);
        // Vector multiply-accumulate 4 float
        acc0_32x4 = vmlaq_f32(acc0_32x4, v0_f32x4, temp);
      }
      // Add the 4 intermediate sum values to get the final dot-prod value for
      // this column.
      *result_in_batch +=
          (vgetq_lane_f32(acc0_32x4, 0) + vgetq_lane_f32(acc0_32x4, 1) +
           vgetq_lane_f32(acc0_32x4, 2) + vgetq_lane_f32(acc0_32x4, 3));
      for (int c = postamble_start; c < m_cols; c++) {
        *result_in_batch += matrix_ptr0[c] * vector_in_batch[c];
      }
      matrix_ptr0 += m_cols;
      result_in_batch += result_stride;
    }
  }
  free(aligned_vector_cache_free);
}

void NeonMatrixBatchVectorMultiplyAccumulate(
    const int8_t* __restrict__ matrix, const int m_rows, const int m_cols,
    const int8_t* __restrict__ vectors, const float* scaling_factors,
    int n_batch, float* __restrict__ result, int result_stride) {
  const int kWeightsPerUint32 = 4;
  const int kWeightsPerNeonLane = 16;
  // If the number of rows is not divisible by kWeightsPerUint32, we set a
  // flag and allocate an aligned memory block. The flag is used to use the
  // aligned memory block later in the kernel loop.
  bool unaligned = false;
  int8* aligned_row = nullptr;
  void* aligned_row_free = nullptr;
  if ((m_cols & (kWeightsPerUint32 - 1)) != 0) {
    unaligned = true;
    aligned_row = (int8*)aligned_alloc(kWeightsPerUint32, m_cols,  // NOLINT
                                       &aligned_row_free);
  }
  void* aligned_vec_free = nullptr;
  int8* aligned_vec = (int8*)aligned_alloc(kWeightsPerUint32, m_cols,  // NOLINT
                                           &aligned_vec_free);

  // If m_cols is not at least kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start = m_cols - (m_cols & (kWeightsPerNeonLane - 1));

  int batch, row, col;
  for (batch = 0; batch < n_batch; ++batch) {
    const float batch_scaling_factor_inv = 1.0 / scaling_factors[batch];
    // Copy the vector data to an aligned vector.
    memcpy(aligned_vec, vectors + batch * m_cols, sizeof(int8) * m_cols);
    // Compute dot-product for every column.
    for (row = 0; row < m_rows; ++row, result += result_stride) {
      // Get the address of the first element of the row.
      int8* row_ptr = (int8*)matrix + row * m_cols;  // NOLINT
      if (unaligned) {
        memcpy(aligned_row, row_ptr, sizeof(int8) * m_cols);
        row_ptr = aligned_row;
      }

      // Initialize the dot product sum for the row to 0.
      int32x4_t dotprod = vmovq_n_s32(0);

      // Prefetch the row to cache.
      __builtin_prefetch(row_ptr, 0 /* prefetch for read */,
                         3 /* temporal locality */);

      // For every block of 16 8-bit elements.
      col = 0;
      for (; col < postamble_start; col += kWeightsPerNeonLane) {
        // Load 16 8-bit values from the row and vector, each, to operate on.
        // Here the assumption is that each buffer is 4-byte aligned.
        TFLITE_CHECK_EQ((uintptr_t)(&row_ptr[col]) & (kWeightsPerUint32 - 1),
                        0);
        const int8x16_t s1_8x16 = vld1q_s8((const int8_t*)(aligned_vec + col));
        const int8x16_t s2_8x16 = vld1q_s8((const int8_t*)(row_ptr + col));
        // Multiply the low bits (i.e. the lower 8 8bit numbers in the
        // registers).
        int16x8_t prod_16x8 =
            vmull_s8(vget_low_s8(s1_8x16), vget_low_s8(s2_8x16));
        // Multiply the high bits (i.e. the lower 8 8bit numbers in the
        // registers), and accumulate with the result of the low bits product.
        // The assumption here is that overflow will not happen as we quantize
        // our values to be in the range [-127, 127]. As such the sum of the 2
        // products is always strictly smaller than 15-bits (32767 in absolute
        // value).
        prod_16x8 =
            vmlal_s8(prod_16x8, vget_high_s8(s1_8x16), vget_high_s8(s2_8x16));

        dotprod = vpadalq_s16(dotprod, prod_16x8);
      }  // for col

      int32 postable_sum = 0;
      // Postamble loop.
      // TODO(raziel): if (ABSL_PREDICT_FALSE(postamble_start < m_rows))
      if (postamble_start < m_cols) {
        col = postamble_start;
        if ((m_cols - postamble_start) >= (kWeightsPerNeonLane >> 1)) {
          // Load 8 8-bit values from the row and column each to operate on.
          // Here the assumption is that each buffer is 4-bytes aligned.
          TFLITE_CHECK_EQ((uintptr_t)(&row_ptr[col]) & (kWeightsPerUint32 - 1),
                          0);
          const int8x8_t s1_8x8 = vld1_s8((const int8_t*)(aligned_vec + col));
          const int8x8_t s2_8x8 = vld1_s8((const int8_t*)(row_ptr + col));
          const int16x8_t prod_16x8 = vmull_s8(s1_8x8, s2_8x8);
          dotprod = vpadalq_s16(dotprod, prod_16x8);
          col += (kWeightsPerNeonLane >> 1);
        }
        for (; col < m_cols; ++col) {
          postable_sum += row_ptr[col] * aligned_vec[col];
        }  // for col
      }
      // Add the 4 intermediate sum values to get the final dot-prod value for
      // this row.
      int64x2_t pairwiseAdded = vpaddlq_s32(dotprod);
      int32 neon_sum =
          vgetq_lane_s64(pairwiseAdded, 0) + vgetq_lane_s64(pairwiseAdded, 1);

      *result += ((neon_sum + postable_sum) * batch_scaling_factor_inv);
    }  // for row
  }    // for batch

  if (unaligned) {
    free(aligned_row_free);
  }
  free(aligned_vec_free);
}

void NeonVectorVectorCwiseProduct(const float* vector1, const float* vector2,
                                  int v_size, float* result) {
  // If v_size is not divisible by kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start =
      v_size - (v_size & (kFloatWeightsPerNeonLane - 1));
  for (int v = 0; v < postamble_start; v += kFloatWeightsPerNeonLane) {
    // Load 4 float values from vector1 and vector2.
    float32x4_t v1_f32x4 = vld1q_f32(vector1 + v);
    float32x4_t v2_f32x4 = vld1q_f32(vector2 + v);
    // Vector multiply 4 float
    float32x4_t mul_32x4 = vmulq_f32(v1_f32x4, v2_f32x4);
    // Save to result array.
    vst1q_f32(&result[v], mul_32x4);
  }
  for (int v = postamble_start; v < v_size; v++) {
    result[v] = vector1[v] * vector2[v];
  }
}

void NeonVectorVectorCwiseProductAccumulate(const float* vector1,
                                            const float* vector2, int v_size,
                                            float* result) {
  // If v_size is not divisible by kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start =
      v_size - (v_size & (kFloatWeightsPerNeonLane - 1));
  for (int v = 0; v < postamble_start; v += kFloatWeightsPerNeonLane) {
    // Load 4 float values from vector1 and vector2 and accumulator.
    float32x4_t v1_f32x4 = vld1q_f32(vector1 + v);
    float32x4_t v2_f32x4 = vld1q_f32(vector2 + v);
    float32x4_t acc_32x4 = vld1q_f32(result + v);
    // Vector multiply-accumulate 4 float
    acc_32x4 = vmlaq_f32(acc_32x4, v1_f32x4, v2_f32x4);
    // Save to result array.
    vst1q_f32(&result[v], acc_32x4);
  }
  for (int v = postamble_start; v < v_size; v++) {
    result[v] += vector1[v] * vector2[v];
  }
}

void NeonVectorBatchVectorCwiseProductAccumulate(const float* vector,
                                                 int v_size,
                                                 const float* batch_vector,
                                                 int n_batch, float* result) {
  // If v_size is not divisible by kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start =
      v_size - (v_size & (kFloatWeightsPerNeonLane - 1));

  // The arrays used to cache the vector.
  void* aligned_vector_cache_free = nullptr;
  float32x4_t* vector_cache_float32x4 =
      reinterpret_cast<float32x4_t*>(aligned_alloc(
          sizeof(float32x4_t), (postamble_start >> 2) * sizeof(float32x4_t),
          &aligned_vector_cache_free));

  for (int v = 0; v < postamble_start; v += kFloatWeightsPerNeonLane) {
    vector_cache_float32x4[v >> 2] = vld1q_f32(vector + v);
  }

  float* result_ptr = result;
  const float* batch_vector_ptr = batch_vector;
  for (int b = 0; b < n_batch; b++) {
    for (int v = 0; v < postamble_start; v += kFloatWeightsPerNeonLane) {
      // Load from memory to vectors.
      float32x4_t result_f32x4 = vld1q_f32(result_ptr + v);
      float32x4_t batch_vector_f32x4 = vld1q_f32(batch_vector_ptr + v);
      // Multiply-accumulate.
      result_f32x4 = vmlaq_f32(result_f32x4, batch_vector_f32x4,
                               vector_cache_float32x4[v >> 2]);
      // Store.
      vst1q_f32(result_ptr + v, result_f32x4);
    }
    // Postamble loop
    for (int v = postamble_start; v < v_size; v++) {
      result_ptr[v] += vector[v] * batch_vector_ptr[v];
    }
    // Update the pointers.
    result_ptr += v_size;
    batch_vector_ptr += v_size;
  }
  free(aligned_vector_cache_free);
}

void NeonSub1Vector(const float* vector, int v_size, float* result) {
  // If v_size is not divisible by kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start =
      v_size - (v_size & (kFloatWeightsPerNeonLane - 1));

  float32x4_t one_f32x4 = vmovq_n_f32(1.0);
  for (int v = 0; v < postamble_start; v += kFloatWeightsPerNeonLane) {
    // Load 4 float values from the current pointers of the input column and
    // subtract from 1.
    float32x4_t v_f32x4 = vld1q_f32(vector + v);
    float32x4_t result_f32x4 = vsubq_f32(one_f32x4, v_f32x4);
    // Save to output.
    vst1q_f32(result + v, result_f32x4);
  }
  for (int v = postamble_start; v < v_size; v++) {
    result[v] = 1.0f - vector[v];
  }
}

void NeonClipVector(const float* vector, int v_size, float abs_limit,
                    float* result) {
  // If v_size is not divisible by kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start =
      v_size - (v_size & (kFloatWeightsPerNeonLane - 1));

  // Replicate abs_limit and -abs_limit in two vectors.
  const float32x4_t abs_limit_f32x4 = vmovq_n_f32(abs_limit);
  const float32x4_t neg_abs_limit_f32x4 = vmovq_n_f32(-abs_limit);

  for (int v = 0; v < postamble_start; v += kFloatWeightsPerNeonLane) {
    // Load from memory to vector.
    float32x4_t v_f32x4 = vld1q_f32(vector + v);
    // Clip between abs_limit and -abs_limit.
    float32x4_t result_f32x4 = vminq_f32(abs_limit_f32x4, v_f32x4);
    result_f32x4 = vmaxq_f32(neg_abs_limit_f32x4, result_f32x4);
    // Save to output.
    vst1q_f32(result + v, result_f32x4);
  }
  // Postamble loop.
  for (int v = postamble_start; v < v_size; v++) {
    result[v] = (abs_limit < vector[v]) ? abs_limit : vector[v];
    result[v] = (-abs_limit > result[v]) ? -abs_limit : result[v];
  }
}

void NeonSymmetricQuantizeFloats(const float* values, const int size,
                                 int8_t* quantized_values, float* min,
                                 float* max, float* scaling_factor) {
  // TODO(raziel): vectorize min/max calculation.
  auto minmax = std::minmax_element(values, values + size);
  *min = *minmax.first;
  *max = *minmax.second;
  const int kScale = 127;
  const float range = std::max(std::abs(*min), std::abs(*max));
  if (range == 0) {
    memset(quantized_values, 0, size * sizeof(int8_t));
    *scaling_factor = 1;
    return;
  }
  *scaling_factor = kScale / range;

  const int postamble_start =
      size - (size & (2 * kFloatWeightsPerNeonLane - 1));

  // Vectorized constants.
  const float32x4_t q_factor_f32x4 = vmovq_n_f32(*scaling_factor);
  const float32x4_t point5_f32x4 = vmovq_n_f32(0.5);
  const float32x4_t zero_f32x4 = vmovq_n_f32(0.0);
  const int32x4_t scale_i32x4 = vmovq_n_s32(kScale);
  const int32x4_t neg_scale_i32x4 = vmovq_n_s32(-kScale);

  for (int i = 0; i < postamble_start; i += 2 * kFloatWeightsPerNeonLane) {
    // Implements the vectorized version of the following:
    // const int32 quantized_value = static_cast<int32>(
    //    std::round(*scaling_factor * values[i]));
    // Since the vectorized round intrinsics (vrndqa_f32) is not supported
    // on all Neon flavors, we use the following method for rounding: if (x
    // < 0) (int)(x - 0.5) if (x >= 0) (int)(x + 0.5)
    float32x4_t value0_f32x4 = vld1q_f32(&values[i]);
    float32x4_t value1_f32x4 = vld1q_f32(&values[i + kFloatWeightsPerNeonLane]);
    float32x4_t mul0_f32x4 = vmulq_f32(value0_f32x4, q_factor_f32x4);
    float32x4_t mul1_f32x4 = vmulq_f32(value1_f32x4, q_factor_f32x4);

    int32x4_t cmp_with_zero0_ui32x4 =
        (int32x4_t)vcltq_f32(mul0_f32x4, zero_f32x4);  // NOLINT
    int32x4_t cmp_with_zero1_ui32x4 =
        (int32x4_t)vcltq_f32(mul1_f32x4, zero_f32x4);  // NOLINT

    float32x4_t cmp_with_zero0_f32x4 = vcvtq_f32_s32(cmp_with_zero0_ui32x4);
    float32x4_t cmp_with_zero1_f32x4 = vcvtq_f32_s32(cmp_with_zero1_ui32x4);
    cmp_with_zero0_f32x4 = vaddq_f32(cmp_with_zero0_f32x4, point5_f32x4);
    cmp_with_zero1_f32x4 = vaddq_f32(cmp_with_zero1_f32x4, point5_f32x4);

    mul0_f32x4 = vaddq_f32(mul0_f32x4, cmp_with_zero0_f32x4);
    mul1_f32x4 = vaddq_f32(mul1_f32x4, cmp_with_zero1_f32x4);

    int32x4_t f2i0_i32x4 = vcvtq_s32_f32(mul0_f32x4);
    int32x4_t f2i1_i32x4 = vcvtq_s32_f32(mul1_f32x4);

    // Implements the vectorized version of the folowing block:
    //  quantized_values[i] = std::min(kScale, std::max(-kScale,
    //  quantized_value));
    int32x4_t max0_i32x4 = vmaxq_s32(f2i0_i32x4, neg_scale_i32x4);
    int32x4_t max1_i32x4 = vmaxq_s32(f2i1_i32x4, neg_scale_i32x4);
    int32x4_t min0_i32x4 = vminq_s32(max0_i32x4, scale_i32x4);
    int32x4_t min1_i32x4 = vminq_s32(max1_i32x4, scale_i32x4);

    int16x4_t min0_16x4 = vmovn_s32(min0_i32x4);
    int16x4_t min1_16x4 = vmovn_s32(min1_i32x4);

    int16x8_t min_16x8 = vcombine_s16(min0_16x4, min1_16x4);
    int8x8_t min_s8x8 = vqmovn_s16(min_16x8);
    vst1_s8(&quantized_values[i], min_s8x8);
  }

  for (int i = postamble_start; i < size; ++i) {
    const int32 quantized_value =
        static_cast<int32>(TfLiteRound(*scaling_factor * values[i]));
    quantized_values[i] = std::min(kScale, std::max(-kScale, quantized_value));
  }
}

float NeonVectorVectorDotProduct(const float* vector1, const float* vector2,
                                 int v_size) {
  // If v_size is not divisible by kWeightsPerNeonLane, we cannot use the main
  // vectorized loop, and we need to process sequentially. postamble_start shows
  // the start index where this should happen.
  const int postamble_start =
      v_size - (v_size & (kFloatWeightsPerNeonLane - 1));
  float32x4_t acc_32x4 = vmovq_n_f32(0.0);
  for (int v = 0; v < postamble_start; v += kFloatWeightsPerNeonLane) {
    // Load 4 float values from vector1 and vector2 and accumulator.
    float32x4_t v1_f32x4 = vld1q_f32(vector1 + v);
    float32x4_t v2_f32x4 = vld1q_f32(vector2 + v);
    // Vector multiply-accumulate 4 float
    acc_32x4 = vmlaq_f32(acc_32x4, v1_f32x4, v2_f32x4);
  }

  float result = (vgetq_lane_f32(acc_32x4, 0) + vgetq_lane_f32(acc_32x4, 1) +
                  vgetq_lane_f32(acc_32x4, 2) + vgetq_lane_f32(acc_32x4, 3));
  // Postamble loop.
  for (int v = postamble_start; v < v_size; v++) {
    result += vector1[v] * vector2[v];
  }
  return result;
}

void NeonBatchVectorBatchVectorDotProduct(const float* vector1,
                                          const float* vector2, int v_size,
                                          int n_batch, float* result,
                                          int result_stride) {
  float* result_ptr = result;
  const float* vector1_ptr = vector1;
  const float* vector2_ptr = vector2;
  for (int b = 0; b < n_batch; b++) {
    *result_ptr = NeonVectorVectorDotProduct(vector1_ptr, vector2_ptr, v_size);
    vector1_ptr += v_size;
    vector2_ptr += v_size;
    result_ptr += result_stride;
  }
}

void NeonReductionSumVector(const float* input_vector, float* output_vector,
                            int output_size, int reduction_size) {
  const float* input_vector_ptr = input_vector;
  for (int o = 0; o < output_size; o++) {
    // If reduction_size is not divisible by kWeightsPerNeonLane, we cannot use
    // the main vectorized loop, and we need to process sequentially.
    // postamble_start shows the start index where this should happen.
    const int postamble_start =
        reduction_size - (reduction_size & (kFloatWeightsPerNeonLane - 1));
    float32x4_t sum_f32x4 = vmovq_n_f32(0.0);
    for (int r = 0; r < postamble_start; r += kFloatWeightsPerNeonLane) {
      float32x4_t v1_f32x4 = vld1q_f32(input_vector_ptr + r);
      sum_f32x4 = vaddq_f32(sum_f32x4, v1_f32x4);
    }
    output_vector[o] +=
        (vgetq_lane_f32(sum_f32x4, 0) + vgetq_lane_f32(sum_f32x4, 1) +
         vgetq_lane_f32(sum_f32x4, 2) + vgetq_lane_f32(sum_f32x4, 3));
    input_vector_ptr += postamble_start;

    // Postamble loop.
    for (int r = postamble_start; r < reduction_size; r++) {
      output_vector[o] += *input_vector_ptr++;
    }
  }
}

void NeonVectorShiftLeft(float* vector, int v_size, float shift_value) {
  // This variable keeps track of the next to the last index which is being
  // copied to make sure we are not out of the vector boundary.
  int last_index_copy = kFloatWeightsPerNeonLane;
  int current_index_copy = 0;
  while (last_index_copy < v_size) {
    float32x4_t v_f32x4 = vld1q_f32(vector + current_index_copy + 1);
    vst1q_f32(vector + current_index_copy, v_f32x4);
    current_index_copy += kFloatWeightsPerNeonLane;
    last_index_copy += kFloatWeightsPerNeonLane;
  }
  // Postamble loop.
  for (int i = current_index_copy; i < v_size - 1; i++) {
    vector[i] = vector[i + 1];
  }
  vector[v_size - 1] = shift_value;
}

}  // namespace tensor_utils
}  // namespace tflite

#endif  // USE_NEON
