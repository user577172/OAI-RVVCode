/*
 * OAI DFT/IDFT RVV kernels.
 *
 * Keep this layer independent from SIMDe fixed-width types.  All entry
 * points operate on ordinary C arrays and use strip-mined vsetvl loops, so
 * the same source is valid for X100, A100 and other RVV implementations.
 */
#ifndef OAI_DFTS_RVV_H
#define OAI_DFTS_RVV_H

#include <stddef.h>
#include <stdint.h>

#if defined(__riscv_vector)
#include <riscv_vector.h>

static inline void
oai_rvv_transpose_4x8_lane128_i32(const int32_t *src, int32_t *dst)
{
  static const uint32_t idx[4][8] = {
    {0,32,64,96,16,48,80,112}, {4,36,68,100,20,52,84,116},
    {8,40,72,104,24,56,88,120}, {12,44,76,108,28,60,92,124}
  };
  const size_t vl = __riscv_vsetvl_e32m1(8);
  for (size_t i = 0; i < 4; ++i) {
    vuint32m1_t off = __riscv_vle32_v_u32m1(idx[i], vl);
    vint32m1_t v = __riscv_vluxei32_v_i32m1(src, off, vl);
    __riscv_vse32_v_i32m1(dst + 8 * i, v, vl);
  }
}

static inline void
oai_rvv_store_dft16x2_i16(int16_t *dst, const int16_t *src,
                          unsigned int shift)
{
  static const uint16_t idx[64] = {
     0, 1, 2, 3, 4, 5, 6, 7, 16,17,18,19,20,21,22,23,
    32,33,34,35,36,37,38,39, 48,49,50,51,52,53,54,55,
     8, 9,10,11,12,13,14,15, 24,25,26,27,28,29,30,31,
    40,41,42,43,44,45,46,47, 56,57,58,59,60,61,62,63
  };
  size_t done = 0;
  while (done < 64) {
    const size_t vl = __riscv_vsetvl_e16m1(64 - done);
    vuint16m1_t element = __riscv_vle16_v_u16m1(idx + done, vl);
    vuint32m2_t byteoff = __riscv_vwmulu_vx_u32m2(element, 2, vl);
    vint16m1_t v = __riscv_vluxei32_v_i16m1(src, byteoff, vl);
    if (shift) v = __riscv_vsra_vx_i16m1(v, shift, vl);
    __riscv_vse16_v_i16m1(dst + done, v, vl);
    done += vl;
  }
}
#endif

/*
 * OAI's mulhi_int16 is a saturating doubling high multiply (vqdmulhq_s16),
 * i.e. an exact arithmetic >> 15 without rounding.  Widen first so this does
 * not depend on vxrm; handle the sole positive overflow case explicitly.
 */
static inline void
oai_dfts_mulhi_i16(int16_t *data, size_t count, int16_t factor)
{
#if defined(__riscv_vector)
  while (count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(count);
    vint16m1_t v = __riscv_vle16_v_i16m1(data, vl);
    vint32m2_t product = __riscv_vwmul_vx_i32m2(v, factor, vl);
    vint32m2_t scaled = __riscv_vsra_vx_i32m2(product, 15, vl);
    vbool16_t overflow = __riscv_vmseq_vx_i32m2_b16(scaled, INT32_C(32768), vl);
    vint16m1_t result = __riscv_vnsra_wx_i16m1(scaled, 0, vl);
    result = __riscv_vmerge_vxm_i16m1(result, INT16_MAX, overflow, vl);
    __riscv_vse16_v_i16m1(data, result, vl);
    data += vl;
    count -= vl;
  }
#else
  for (size_t i = 0; i < count; ++i)
  {
    int32_t scaled = ((int32_t)data[i] * factor) >> 15;
    data[i] = (int16_t)(scaled > INT16_MAX ? INT16_MAX : scaled);
  }
#endif
}

/* PMULHRSW-compatible Q15 multiply: add the rounding bias before the
 * arithmetic shift and saturate the exceptional positive overflow. */
static inline void
oai_dfts_mulhrs_i16(int16_t *data, size_t count, int16_t factor)
{
#if defined(__riscv_vector)
  while (count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(count);
    vint16m1_t value = __riscv_vle16_v_i16m1(data, vl);
    vint32m2_t product = __riscv_vwmul_vx_i32m2(value, factor, vl);
    product = __riscv_vadd_vx_i32m2(product, INT32_C(16384), vl);
    product = __riscv_vsra_vx_i32m2(product, 15, vl);
    product = __riscv_vmax_vx_i32m2(product, INT16_MIN, vl);
    product = __riscv_vmin_vx_i32m2(product, INT16_MAX, vl);
    vint16m1_t result = __riscv_vnsra_wx_i16m1(product, 0, vl);
    __riscv_vse16_v_i16m1(data, result, vl);
    data += vl;
    count -= vl;
  }
#else
  for (size_t i = 0; i < count; ++i) {
    int32_t result = (((int32_t)data[i] * factor) + 16384) >> 15;
    if (result > INT16_MAX) result = INT16_MAX;
    if (result < INT16_MIN) result = INT16_MIN;
    data[i] = (int16_t)result;
  }
#endif
}

static inline void
oai_dfts_mulhrs_copy_i16(const int16_t *src, int16_t *dst,
                         size_t count, int16_t factor)
{
#if defined(__riscv_vector)
  while (count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(count);
    vint16m1_t value = __riscv_vle16_v_i16m1(src, vl);
    vint32m2_t product = __riscv_vwmul_vx_i32m2(value, factor, vl);
    product = __riscv_vadd_vx_i32m2(product, INT32_C(16384), vl);
    product = __riscv_vsra_vx_i32m2(product, 15, vl);
    product = __riscv_vmax_vx_i32m2(product, INT16_MIN, vl);
    product = __riscv_vmin_vx_i32m2(product, INT16_MAX, vl);
    __riscv_vse16_v_i16m1(dst, __riscv_vnsra_wx_i16m1(product, 0, vl), vl);
    src += vl;
    dst += vl;
    count -= vl;
  }
#else
  for (size_t i = 0; i < count; ++i) {
    int32_t result = (((int32_t)src[i] * factor) + 16384) >> 15;
    if (result > INT16_MAX) result = INT16_MAX;
    if (result < INT16_MIN) result = INT16_MIN;
    dst[i] = (int16_t)result;
  }
#endif
}

#if defined(__riscv_vector)

static inline vint16m1_t
oai_rvv_rotate_minus_j_i16(vint16m1_t value, size_t vl)
{
  vuint16m1_t lane = __riscv_vid_v_u16m1(vl);
  vuint16m1_t peer = __riscv_vxor_vx_u16m1(lane, 1, vl);
  vint16m1_t swapped = __riscv_vrgather_vv_i16m1(value, peer, vl);
  vint16m1_t negated = __riscv_vneg_v_i16m1(swapped, vl);
  vbool16_t odd = __riscv_vmsne_vx_u16m1_b16(
      __riscv_vand_vx_u16m1(lane, 1, vl), 0, vl);
  return __riscv_vmerge_vvm_i16m1(swapped, negated, odd, vl);
}

static inline void
oai_rvv_bfly2_tw1_i16(const int16_t *x0, const int16_t *x1,
                      int16_t *y0, int16_t *y1, size_t count)
{
  while (count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(count);
    vint16m1_t a = __riscv_vle16_v_i16m1(x0, vl);
    vint16m1_t b = __riscv_vle16_v_i16m1(x1, vl);
    vint16m1_t sum = __riscv_vsadd_vv_i16m1(a, b, vl);
    vint16m1_t difference = __riscv_vssub_vv_i16m1(a, b, vl);
    __riscv_vse16_v_i16m1(y0, sum, vl);
    __riscv_vse16_v_i16m1(y1, difference, vl);
    x0 += vl; x1 += vl; y0 += vl; y1 += vl;
    count -= vl;
  }
}

static inline vint16m1_t
oai_rvv_q15_dot2_i16(vint16m1_t a0, vint16m1_t a1,
                     vint16m1_t b0, vint16m1_t b1, size_t vl)
{
  vint32m2_t product = __riscv_vwmul_vv_i32m2(a0, b0, vl);
  product = __riscv_vwmacc_vv_i32m2(product, a1, b1, vl);
  product = __riscv_vsra_vx_i32m2(product, 15, vl);
  product = __riscv_vmax_vx_i32m2(product, INT16_MIN, vl);
  product = __riscv_vmin_vx_i32m2(product, INT16_MAX, vl);
  return __riscv_vnsra_wx_i16m1(product, 0, vl);
}

/* Fused radix-2 Q15 stage.  Keep arithmetic in 32 bits until the final
 * shift/pack, matching OAI's madd/srai/pack semantics without SIMDe objects. */
static inline void
oai_rvv_bfly2_q15_i16(const int16_t *x0, const int16_t *x1,
                      const int16_t *tw, int16_t *y0, int16_t *y1,
                      size_t complex_count, int inverse)
{
  const ptrdiff_t stride = 2 * (ptrdiff_t)sizeof(int16_t);
  while (complex_count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(complex_count);
    vint16m1_t a0r = __riscv_vlse16_v_i16m1(x0, stride, vl);
    vint16m1_t a0i = __riscv_vlse16_v_i16m1(x0 + 1, stride, vl);
    vint16m1_t a1r = __riscv_vlse16_v_i16m1(x1, stride, vl);
    vint16m1_t a1i = __riscv_vlse16_v_i16m1(x1 + 1, stride, vl);
    vint16m1_t wr = __riscv_vlse16_v_i16m1(tw, stride, vl);
    vint16m1_t wi = __riscv_vlse16_v_i16m1(tw + 1, stride, vl);
    vint32m2_t x0r = __riscv_vwmul_vx_i32m2(a0r, INT16_MAX, vl);
    vint32m2_t x0i = __riscv_vwmul_vx_i32m2(a0i, INT16_MAX, vl);
    vint32m2_t x1r = __riscv_vwmul_vv_i32m2(a1r, wr, vl);
    vint32m2_t x1i;
    if (inverse) {
      x1r = __riscv_vwmacc_vv_i32m2(x1r, a1i, wi, vl);
      x1i = __riscv_vwmul_vv_i32m2(a1i, wr, vl);
      x1i = __riscv_vsub_vv_i32m2(
          x1i, __riscv_vwmul_vv_i32m2(a1r, wi, vl), vl);
    } else {
      x1r = __riscv_vsub_vv_i32m2(
          x1r, __riscv_vwmul_vv_i32m2(a1i, wi, vl), vl);
      x1i = __riscv_vwmul_vv_i32m2(a1r, wi, vl);
      x1i = __riscv_vwmacc_vv_i32m2(x1i, a1i, wr, vl);
    }
#define OAI_RVV_Q15_PACK(value)                                                \
    __riscv_vnsra_wx_i16m1(                                                   \
        __riscv_vmin_vx_i32m2(                                                \
            __riscv_vmax_vx_i32m2(                                           \
                __riscv_vsra_vx_i32m2((value), 15, vl), INT16_MIN, vl),       \
            INT16_MAX, vl), 0, vl)
    vint16m1_t y0r = OAI_RVV_Q15_PACK(__riscv_vadd_vv_i32m2(x0r, x1r, vl));
    vint16m1_t y0i = OAI_RVV_Q15_PACK(__riscv_vadd_vv_i32m2(x0i, x1i, vl));
    vint16m1_t y1r = OAI_RVV_Q15_PACK(__riscv_vsub_vv_i32m2(x0r, x1r, vl));
    vint16m1_t y1i = OAI_RVV_Q15_PACK(__riscv_vsub_vv_i32m2(x0i, x1i, vl));
#undef OAI_RVV_Q15_PACK
    __riscv_vsse16_v_i16m1(y0, stride, y0r, vl);
    __riscv_vsse16_v_i16m1(y0 + 1, stride, y0i, vl);
    __riscv_vsse16_v_i16m1(y1, stride, y1r, vl);
    __riscv_vsse16_v_i16m1(y1 + 1, stride, y1i, vl);
    const size_t step = 2 * vl;
    x0 += step; x1 += step; tw += step; y0 += step; y1 += step;
    complex_count -= vl;
  }
}

/* Native 2048-point radix-2 FFT.  The first four stages vectorize across
 * independent blocks; later stages vectorize contiguous butterflies inside
 * each block.  No fixed-width x86/SIMDe object is part of this data flow. */
static inline void
oai_rvv_fft2048_i16(const int16_t *input, int16_t *output,
                    const int16_t *twiddle, const uint32_t *bitrev,
                    int inverse, int final_scale)
{
  int16_t data[4096] __attribute__((aligned(64)));

  /* Bit-reversed input, gathered as packed complex int32 values. */
  size_t done = 0;
  while (done < 2048) {
    const size_t vl = __riscv_vsetvl_e32m1(2048 - done);
    vuint32m1_t index = __riscv_vle32_v_u32m1(bitrev + done, vl);
    index = __riscv_vsll_vx_u32m1(index, 2, vl);
    vint32m1_t value = __riscv_vluxei32_v_i32m1((const int32_t *)input,
                                                 index, vl);
    __riscv_vse32_v_i32m1((int32_t *)data + done, value, vl);
    done += vl;
  }

  for (unsigned int stage = 1, width = 2; stage <= 11;
       ++stage, width <<= 1) {
    const unsigned int half = width >> 1;
    const unsigned int tw_step = 2048 / width;
    const unsigned int qshift = stage <= 5 ? 16 : 15;

    if (stage <= 4) {
      /* Same butterfly position from many independent blocks per vector. */
      const size_t blocks = 2048 / width;
      const ptrdiff_t block_stride = (ptrdiff_t)width * 2 * sizeof(int16_t);
      for (unsigned int j = 0; j < half; ++j) {
        const int16_t wr = twiddle[2 * j * tw_step];
        const int16_t wi0 = twiddle[2 * j * tw_step + 1];
        const int16_t wi = inverse ? (int16_t)-wi0 : wi0;
        size_t block = 0;
        while (block < blocks) {
          const size_t vl = __riscv_vsetvl_e16m1(blocks - block);
          int16_t *a = data + 2 * (block * width + j);
          int16_t *b = a + 2 * half;
          vint16m1_t ar = __riscv_vlse16_v_i16m1(a, block_stride, vl);
          vint16m1_t ai = __riscv_vlse16_v_i16m1(a + 1, block_stride, vl);
          vint16m1_t br = __riscv_vlse16_v_i16m1(b, block_stride, vl);
          vint16m1_t bi = __riscv_vlse16_v_i16m1(b + 1, block_stride, vl);
          vint32m2_t tr = __riscv_vwmul_vx_i32m2(br, wr, vl);
          tr = __riscv_vsub_vv_i32m2(
              tr, __riscv_vwmul_vx_i32m2(bi, wi, vl), vl);
          vint32m2_t ti = __riscv_vwmul_vx_i32m2(br, wi, vl);
          ti = __riscv_vwmacc_vx_i32m2(ti, wr, bi, vl);
          vint32m2_t aq_r = __riscv_vwmul_vx_i32m2(ar, INT16_MAX, vl);
          vint32m2_t aq_i = __riscv_vwmul_vx_i32m2(ai, INT16_MAX, vl);
#define OAI_RVV_FFT_PACK(v)                                                    \
          __riscv_vnsra_wx_i16m1(                                             \
              __riscv_vmin_vx_i32m2(                                          \
                  __riscv_vmax_vx_i32m2(                                      \
                      __riscv_vsra_vx_i32m2(                                  \
                          __riscv_vadd_vx_i32m2(                               \
                              (v), INT32_C(1) << (qshift - 1), vl),            \
                          qshift, vl), INT16_MIN, vl),                         \
                  INT16_MAX, vl), 0, vl)
          vint16m1_t o0r = OAI_RVV_FFT_PACK(__riscv_vadd_vv_i32m2(aq_r,tr,vl));
          vint16m1_t o0i = OAI_RVV_FFT_PACK(__riscv_vadd_vv_i32m2(aq_i,ti,vl));
          vint16m1_t o1r = OAI_RVV_FFT_PACK(__riscv_vsub_vv_i32m2(aq_r,tr,vl));
          vint16m1_t o1i = OAI_RVV_FFT_PACK(__riscv_vsub_vv_i32m2(aq_i,ti,vl));
#undef OAI_RVV_FFT_PACK
          __riscv_vsse16_v_i16m1(a, block_stride, o0r, vl);
          __riscv_vsse16_v_i16m1(a + 1, block_stride, o0i, vl);
          __riscv_vsse16_v_i16m1(b, block_stride, o1r, vl);
          __riscv_vsse16_v_i16m1(b + 1, block_stride, o1i, vl);
          block += vl;
        }
      }
    } else {
      for (unsigned int base = 0; base < 2048; base += width) {
        size_t j = 0;
        while (j < half) {
          const size_t vl = __riscv_vsetvl_e16m1(half - j);
          int16_t *a = data + 2 * (base + j);
          int16_t *b = a + 2 * half;
          const ptrdiff_t complex_stride = 2 * (ptrdiff_t)sizeof(int16_t);
          const ptrdiff_t tw_stride = (ptrdiff_t)tw_step * complex_stride;
          const int16_t *tw = twiddle + 2 * j * tw_step;
          vint16m1_t ar = __riscv_vlse16_v_i16m1(a, complex_stride, vl);
          vint16m1_t ai = __riscv_vlse16_v_i16m1(a + 1, complex_stride, vl);
          vint16m1_t br = __riscv_vlse16_v_i16m1(b, complex_stride, vl);
          vint16m1_t bi = __riscv_vlse16_v_i16m1(b + 1, complex_stride, vl);
          vint16m1_t wr = __riscv_vlse16_v_i16m1(tw, tw_stride, vl);
          vint16m1_t wi = __riscv_vlse16_v_i16m1(tw + 1, tw_stride, vl);
          if (inverse) wi = __riscv_vneg_v_i16m1(wi, vl);
          vint32m2_t tr = __riscv_vwmul_vv_i32m2(br, wr, vl);
          tr = __riscv_vsub_vv_i32m2(
              tr, __riscv_vwmul_vv_i32m2(bi, wi, vl), vl);
          vint32m2_t ti = __riscv_vwmul_vv_i32m2(br, wi, vl);
          ti = __riscv_vwmacc_vv_i32m2(ti, bi, wr, vl);
          vint32m2_t aq_r = __riscv_vwmul_vx_i32m2(ar, INT16_MAX, vl);
          vint32m2_t aq_i = __riscv_vwmul_vx_i32m2(ai, INT16_MAX, vl);
#define OAI_RVV_FFT_PACK(v)                                                    \
          __riscv_vnsra_wx_i16m1(                                             \
              __riscv_vmin_vx_i32m2(                                          \
                  __riscv_vmax_vx_i32m2(                                      \
                      __riscv_vsra_vx_i32m2(                                  \
                          __riscv_vadd_vx_i32m2(                               \
                              (v), INT32_C(1) << (qshift - 1), vl),            \
                          qshift, vl), INT16_MIN, vl),                         \
                  INT16_MAX, vl), 0, vl)
          vint16m1_t o0r = OAI_RVV_FFT_PACK(__riscv_vadd_vv_i32m2(aq_r,tr,vl));
          vint16m1_t o0i = OAI_RVV_FFT_PACK(__riscv_vadd_vv_i32m2(aq_i,ti,vl));
          vint16m1_t o1r = OAI_RVV_FFT_PACK(__riscv_vsub_vv_i32m2(aq_r,tr,vl));
          vint16m1_t o1i = OAI_RVV_FFT_PACK(__riscv_vsub_vv_i32m2(aq_i,ti,vl));
#undef OAI_RVV_FFT_PACK
          __riscv_vsse16_v_i16m1(a, complex_stride, o0r, vl);
          __riscv_vsse16_v_i16m1(a + 1, complex_stride, o0i, vl);
          __riscv_vsse16_v_i16m1(b, complex_stride, o1r, vl);
          __riscv_vsse16_v_i16m1(b + 1, complex_stride, o1i, vl);
          j += vl;
        }
      }
    }
  }
  if (final_scale)
    oai_dfts_mulhrs_i16(data, 4096, 23170);
  done = 0;
  while (done < 2048) {
    const size_t vl = __riscv_vsetvl_e32m1(2048 - done);
    vint32m1_t value = __riscv_vle32_v_i32m1((const int32_t *)data + done, vl);
    __riscv_vse32_v_i32m1((int32_t *)output + done, value, vl);
    done += vl;
  }
}

/* Packed-complex variant: one {int16 real,int16 imag} sample per e32 lane.
 * Keeping every butterfly in e32,m1 avoids the repeated e16/m1 <-> e32/m2
 * state changes required by widening intrinsics. */
static inline void
oai_rvv_fft2048_packed_i32(const int16_t *input, int16_t *output,
                           const int16_t *twiddle, const uint32_t *bitrev,
                           int inverse, int final_scale,
                           uint16_t prefix_samples)
{
  int32_t data[2048] __attribute__((aligned(64)));

#define OAI_RVV_UNPACK_RE(name, packed, vl_)                                  \
  vint32m1_t name = __riscv_vsra_vx_i32m1(                                   \
      __riscv_vsll_vx_i32m1((packed), 16, (vl_)), 16, (vl_))
#define OAI_RVV_UNPACK_IM(name, packed, vl_)                                  \
  vint32m1_t name = __riscv_vsra_vx_i32m1((packed), 16, (vl_))
#define OAI_RVV_FFT32_PACK(value, shift_, vl_)                                \
  __riscv_vmin_vx_i32m1(                                                      \
      __riscv_vmax_vx_i32m1(                                                 \
          __riscv_vsra_vx_i32m1(                                             \
              __riscv_vadd_vx_i32m1(                                         \
                  (value), INT32_C(1) << ((shift_) - 1), (vl_)),              \
              (shift_), (vl_)), INT16_MIN, (vl_)),                            \
      INT16_MAX, (vl_))
#define OAI_RVV_PACK_COMPLEX(re_, im_, vl_)                                   \
  __riscv_vreinterpret_v_u32m1_i32m1(                                        \
      __riscv_vor_vv_u32m1(                                                  \
          __riscv_vand_vx_u32m1(                                             \
              __riscv_vreinterpret_v_i32m1_u32m1((re_)), UINT32_C(0xffff),   \
              (vl_)),                                                        \
          __riscv_vsll_vx_u32m1(                                             \
              __riscv_vreinterpret_v_i32m1_u32m1((im_)), 16, (vl_)),         \
          (vl_)))

  /* Fuse the first two scaled radix-2 passes into one radix-4 pass. */
  {
    const size_t blocks = 512;
    const ptrdiff_t stride = 4 * (ptrdiff_t)sizeof(int32_t);
    size_t block = 0;
    while (block < blocks) {
      const size_t vl = __riscv_vsetvl_e32m1(blocks - block);
      int32_t *base = data + 4 * block;
      const uint32_t *rev = bitrev + 4 * block;
      vuint32m1_t i0=__riscv_vlse32_v_u32m1(rev+0,stride,vl);
      vuint32m1_t i1=__riscv_vlse32_v_u32m1(rev+1,stride,vl);
      vuint32m1_t i2=__riscv_vlse32_v_u32m1(rev+2,stride,vl);
      vuint32m1_t i3=__riscv_vlse32_v_u32m1(rev+3,stride,vl);
      i0=__riscv_vsll_vx_u32m1(i0,2,vl); i1=__riscv_vsll_vx_u32m1(i1,2,vl);
      i2=__riscv_vsll_vx_u32m1(i2,2,vl); i3=__riscv_vsll_vx_u32m1(i3,2,vl);
      vint32m1_t x0p=__riscv_vluxei32_v_i32m1((const int32_t *)input,i0,vl);
      vint32m1_t x1p=__riscv_vluxei32_v_i32m1((const int32_t *)input,i1,vl);
      vint32m1_t x2p=__riscv_vluxei32_v_i32m1((const int32_t *)input,i2,vl);
      vint32m1_t x3p=__riscv_vluxei32_v_i32m1((const int32_t *)input,i3,vl);
      OAI_RVV_UNPACK_RE(x0r,x0p,vl); OAI_RVV_UNPACK_IM(x0i,x0p,vl);
      OAI_RVV_UNPACK_RE(x1r,x1p,vl); OAI_RVV_UNPACK_IM(x1i,x1p,vl);
      OAI_RVV_UNPACK_RE(x2r,x2p,vl); OAI_RVV_UNPACK_IM(x2i,x2p,vl);
      OAI_RVV_UNPACK_RE(x3r,x3p,vl); OAI_RVV_UNPACK_IM(x3i,x3p,vl);
#define OAI_RVV_RAD2_FIRST(out_, lhs_, rhs_, op_)                             \
      vint32m1_t out_ = OAI_RVV_FFT32_PACK(                                   \
          __riscv_##op_##_vv_i32m1(                                           \
              __riscv_vmul_vx_i32m1((lhs_),INT16_MAX,vl),                    \
              __riscv_vmul_vx_i32m1((rhs_),INT16_MAX,vl),vl),16,vl)
      OAI_RVV_RAD2_FIRST(p0r,x0r,x1r,vadd); OAI_RVV_RAD2_FIRST(p0i,x0i,x1i,vadd);
      OAI_RVV_RAD2_FIRST(p1r,x0r,x1r,vsub); OAI_RVV_RAD2_FIRST(p1i,x0i,x1i,vsub);
      OAI_RVV_RAD2_FIRST(p2r,x2r,x3r,vadd); OAI_RVV_RAD2_FIRST(p2i,x2i,x3i,vadd);
      OAI_RVV_RAD2_FIRST(p3r,x2r,x3r,vsub); OAI_RVV_RAD2_FIRST(p3i,x2i,x3i,vsub);
#undef OAI_RVV_RAD2_FIRST
      vint32m1_t p0qr=__riscv_vmul_vx_i32m1(p0r,INT16_MAX,vl);
      vint32m1_t p0qi=__riscv_vmul_vx_i32m1(p0i,INT16_MAX,vl);
      vint32m1_t p1qr=__riscv_vmul_vx_i32m1(p1r,INT16_MAX,vl);
      vint32m1_t p1qi=__riscv_vmul_vx_i32m1(p1i,INT16_MAX,vl);
      vint32m1_t p2qr=__riscv_vmul_vx_i32m1(p2r,INT16_MAX,vl);
      vint32m1_t p2qi=__riscv_vmul_vx_i32m1(p2i,INT16_MAX,vl);
      vint32m1_t jp3r=__riscv_vmul_vx_i32m1(p3i,inverse ? -INT16_MAX : INT16_MAX,vl);
      vint32m1_t jp3i=__riscv_vmul_vx_i32m1(p3r,inverse ? INT16_MAX : -INT16_MAX,vl);
      vint32m1_t y0r=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(p0qr,p2qr,vl),16,vl);
      vint32m1_t y0i=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(p0qi,p2qi,vl),16,vl);
      vint32m1_t y2r=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(p0qr,p2qr,vl),16,vl);
      vint32m1_t y2i=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(p0qi,p2qi,vl),16,vl);
      vint32m1_t y1r=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(p1qr,jp3r,vl),16,vl);
      vint32m1_t y1i=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(p1qi,jp3i,vl),16,vl);
      vint32m1_t y3r=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(p1qr,jp3r,vl),16,vl);
      vint32m1_t y3i=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(p1qi,jp3i,vl),16,vl);
      __riscv_vsse32_v_i32m1(base+0,stride,OAI_RVV_PACK_COMPLEX(y0r,y0i,vl),vl);
      __riscv_vsse32_v_i32m1(base+1,stride,OAI_RVV_PACK_COMPLEX(y1r,y1i,vl),vl);
      __riscv_vsse32_v_i32m1(base+2,stride,OAI_RVV_PACK_COMPLEX(y2r,y2i,vl),vl);
      __riscv_vsse32_v_i32m1(base+3,stride,OAI_RVV_PACK_COMPLEX(y3r,y3i,vl),vl);
      block += vl;
    }
  }

  /* Fuse stages three and four.  Each lane handles one independent
   * 16-point group while j selects one of four radix-4 columns. */
  {
    const size_t blocks = 128;
    const ptrdiff_t stride = 16 * (ptrdiff_t)sizeof(int32_t);
    for (unsigned int j = 0; j < 4; ++j) {
      const int32_t w8r = twiddle[2 * j * 256];
      const int32_t w8i0 = twiddle[2 * j * 256 + 1];
      const int32_t w8i = inverse ? -w8i0 : w8i0;
      const int32_t w16ar = twiddle[2 * j * 128];
      const int32_t w16ai0 = twiddle[2 * j * 128 + 1];
      const int32_t w16ai = inverse ? -w16ai0 : w16ai0;
      const int32_t w16br = twiddle[2 * (j + 4) * 128];
      const int32_t w16bi0 = twiddle[2 * (j + 4) * 128 + 1];
      const int32_t w16bi = inverse ? -w16bi0 : w16bi0;
      size_t block = 0;
      while (block < blocks) {
        const size_t vl = __riscv_vsetvl_e32m1(blocks - block);
        int32_t *base = data + 16 * block + j;
        vint32m1_t x0p=__riscv_vlse32_v_i32m1(base+0,stride,vl);
        vint32m1_t x1p=__riscv_vlse32_v_i32m1(base+4,stride,vl);
        vint32m1_t x2p=__riscv_vlse32_v_i32m1(base+8,stride,vl);
        vint32m1_t x3p=__riscv_vlse32_v_i32m1(base+12,stride,vl);
        OAI_RVV_UNPACK_RE(x0r,x0p,vl); OAI_RVV_UNPACK_IM(x0i,x0p,vl);
        OAI_RVV_UNPACK_RE(x1r,x1p,vl); OAI_RVV_UNPACK_IM(x1i,x1p,vl);
        OAI_RVV_UNPACK_RE(x2r,x2p,vl); OAI_RVV_UNPACK_IM(x2i,x2p,vl);
        OAI_RVV_UNPACK_RE(x3r,x3p,vl); OAI_RVV_UNPACK_IM(x3i,x3p,vl);
#define OAI_RVV_CMUL_SCALAR(prefix_, xr_, xi_, wr_, wi_)                      \
        vint32m1_t prefix_##r = __riscv_vsub_vv_i32m1(                       \
            __riscv_vmul_vx_i32m1((xr_),(wr_),vl),                           \
            __riscv_vmul_vx_i32m1((xi_),(wi_),vl),vl);                       \
        vint32m1_t prefix_##i = __riscv_vadd_vv_i32m1(                       \
            __riscv_vmul_vx_i32m1((xr_),(wi_),vl),                           \
            __riscv_vmul_vx_i32m1((xi_),(wr_),vl),vl)
#define OAI_RVV_RAD2_Q15(prefix0_,prefix1_,ar_,ai_,tr_,ti_)                   \
        vint32m1_t prefix0_##r=OAI_RVV_FFT32_PACK(                            \
            __riscv_vadd_vv_i32m1(                                           \
                __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),16,vl);  \
        vint32m1_t prefix0_##i=OAI_RVV_FFT32_PACK(                            \
            __riscv_vadd_vv_i32m1(                                           \
                __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),16,vl);  \
        vint32m1_t prefix1_##r=OAI_RVV_FFT32_PACK(                            \
            __riscv_vsub_vv_i32m1(                                           \
                __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),16,vl);  \
        vint32m1_t prefix1_##i=OAI_RVV_FFT32_PACK(                            \
            __riscv_vsub_vv_i32m1(                                           \
                __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),16,vl)
        OAI_RVV_CMUL_SCALAR(t1,x1r,x1i,w8r,w8i);
        OAI_RVV_CMUL_SCALAR(t3,x3r,x3i,w8r,w8i);
        OAI_RVV_RAD2_Q15(p0,p1,x0r,x0i,t1r,t1i);
        OAI_RVV_RAD2_Q15(p2,p3,x2r,x2i,t3r,t3i);
        OAI_RVV_CMUL_SCALAR(q2,p2r,p2i,w16ar,w16ai);
        OAI_RVV_CMUL_SCALAR(q3,p3r,p3i,w16br,w16bi);
        OAI_RVV_RAD2_Q15(y0,y2,p0r,p0i,q2r,q2i);
        OAI_RVV_RAD2_Q15(y1,y3,p1r,p1i,q3r,q3i);
#undef OAI_RVV_RAD2_Q15
#undef OAI_RVV_CMUL_SCALAR
        __riscv_vsse32_v_i32m1(base+0,stride,OAI_RVV_PACK_COMPLEX(y0r,y0i,vl),vl);
        __riscv_vsse32_v_i32m1(base+4,stride,OAI_RVV_PACK_COMPLEX(y1r,y1i,vl),vl);
        __riscv_vsse32_v_i32m1(base+8,stride,OAI_RVV_PACK_COMPLEX(y2r,y2i,vl),vl);
        __riscv_vsse32_v_i32m1(base+12,stride,OAI_RVV_PACK_COMPLEX(y3r,y3i,vl),vl);
        block += vl;
      }
    }
  }

  /* Fuse stages five (scaled) and six (unscaled). */
  {
    const size_t blocks = 32;
    const ptrdiff_t stride = 64 * (ptrdiff_t)sizeof(int32_t);
    for (unsigned int j = 0; j < 16; ++j) {
      const int32_t w32r=twiddle[2*j*64];
      const int32_t w32i0=twiddle[2*j*64+1];
      const int32_t w32i=inverse ? -w32i0 : w32i0;
      const int32_t w64ar=twiddle[2*j*32];
      const int32_t w64ai0=twiddle[2*j*32+1];
      const int32_t w64ai=inverse ? -w64ai0 : w64ai0;
      const int32_t w64br=twiddle[2*(j+16)*32];
      const int32_t w64bi0=twiddle[2*(j+16)*32+1];
      const int32_t w64bi=inverse ? -w64bi0 : w64bi0;
      size_t block=0;
      while (block < blocks) {
        const size_t vl=__riscv_vsetvl_e32m1(blocks-block);
        int32_t *base=data+64*block+j;
        vint32m1_t x0p=__riscv_vlse32_v_i32m1(base+0,stride,vl);
        vint32m1_t x1p=__riscv_vlse32_v_i32m1(base+16,stride,vl);
        vint32m1_t x2p=__riscv_vlse32_v_i32m1(base+32,stride,vl);
        vint32m1_t x3p=__riscv_vlse32_v_i32m1(base+48,stride,vl);
        OAI_RVV_UNPACK_RE(x0r,x0p,vl); OAI_RVV_UNPACK_IM(x0i,x0p,vl);
        OAI_RVV_UNPACK_RE(x1r,x1p,vl); OAI_RVV_UNPACK_IM(x1i,x1p,vl);
        OAI_RVV_UNPACK_RE(x2r,x2p,vl); OAI_RVV_UNPACK_IM(x2i,x2p,vl);
        OAI_RVV_UNPACK_RE(x3r,x3p,vl); OAI_RVV_UNPACK_IM(x3i,x3p,vl);
#define OAI_RVV_CMUL_S(prefix_,xr_,xi_,wr_,wi_)                              \
        vint32m1_t prefix_##r=__riscv_vsub_vv_i32m1(                         \
            __riscv_vmul_vx_i32m1((xr_),(wr_),vl),                           \
            __riscv_vmul_vx_i32m1((xi_),(wi_),vl),vl);                       \
        vint32m1_t prefix_##i=__riscv_vadd_vv_i32m1(                         \
            __riscv_vmul_vx_i32m1((xr_),(wi_),vl),                           \
            __riscv_vmul_vx_i32m1((xi_),(wr_),vl),vl)
#define OAI_RVV_RAD2_S(prefix0_,prefix1_,ar_,ai_,tr_,ti_,shift_)              \
        vint32m1_t prefix0_##r=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(     \
            __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),shift_,vl);  \
        vint32m1_t prefix0_##i=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(     \
            __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),shift_,vl);  \
        vint32m1_t prefix1_##r=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(     \
            __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),shift_,vl);  \
        vint32m1_t prefix1_##i=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(     \
            __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),shift_,vl)
        OAI_RVV_CMUL_S(t1,x1r,x1i,w32r,w32i);
        OAI_RVV_CMUL_S(t3,x3r,x3i,w32r,w32i);
        OAI_RVV_RAD2_S(p0,p1,x0r,x0i,t1r,t1i,16);
        OAI_RVV_RAD2_S(p2,p3,x2r,x2i,t3r,t3i,16);
        OAI_RVV_CMUL_S(q2,p2r,p2i,w64ar,w64ai);
        OAI_RVV_CMUL_S(q3,p3r,p3i,w64br,w64bi);
        OAI_RVV_RAD2_S(y0,y2,p0r,p0i,q2r,q2i,15);
        OAI_RVV_RAD2_S(y1,y3,p1r,p1i,q3r,q3i,15);
#undef OAI_RVV_RAD2_S
#undef OAI_RVV_CMUL_S
        __riscv_vsse32_v_i32m1(base+0,stride,OAI_RVV_PACK_COMPLEX(y0r,y0i,vl),vl);
        __riscv_vsse32_v_i32m1(base+16,stride,OAI_RVV_PACK_COMPLEX(y1r,y1i,vl),vl);
        __riscv_vsse32_v_i32m1(base+32,stride,OAI_RVV_PACK_COMPLEX(y2r,y2i,vl),vl);
        __riscv_vsse32_v_i32m1(base+48,stride,OAI_RVV_PACK_COMPLEX(y3r,y3i,vl),vl);
        block+=vl;
      }
    }
  }

  /* Fuse unscaled stages seven and eight. */
  {
    const size_t blocks=8;
    const ptrdiff_t stride=256*(ptrdiff_t)sizeof(int32_t);
    for (unsigned int j=0;j<64;++j) {
      const int32_t w128r=twiddle[2*j*16];
      const int32_t w128i0=twiddle[2*j*16+1];
      const int32_t w128i=inverse ? -w128i0 : w128i0;
      const int32_t w256ar=twiddle[2*j*8];
      const int32_t w256ai0=twiddle[2*j*8+1];
      const int32_t w256ai=inverse ? -w256ai0 : w256ai0;
      const int32_t w256br=twiddle[2*(j+64)*8];
      const int32_t w256bi0=twiddle[2*(j+64)*8+1];
      const int32_t w256bi=inverse ? -w256bi0 : w256bi0;
      size_t block=0;
      while (block < blocks) {
      const size_t vl=__riscv_vsetvl_e32m1(blocks-block);
      int32_t *base=data+256*block+j;
      vint32m1_t x0p=__riscv_vlse32_v_i32m1(base+0,stride,vl);
      vint32m1_t x1p=__riscv_vlse32_v_i32m1(base+64,stride,vl);
      vint32m1_t x2p=__riscv_vlse32_v_i32m1(base+128,stride,vl);
      vint32m1_t x3p=__riscv_vlse32_v_i32m1(base+192,stride,vl);
      OAI_RVV_UNPACK_RE(x0r,x0p,vl); OAI_RVV_UNPACK_IM(x0i,x0p,vl);
      OAI_RVV_UNPACK_RE(x1r,x1p,vl); OAI_RVV_UNPACK_IM(x1i,x1p,vl);
      OAI_RVV_UNPACK_RE(x2r,x2p,vl); OAI_RVV_UNPACK_IM(x2i,x2p,vl);
      OAI_RVV_UNPACK_RE(x3r,x3p,vl); OAI_RVV_UNPACK_IM(x3i,x3p,vl);
#define OAI_RVV_CMUL_U(prefix_,xr_,xi_,wr_,wi_)                              \
      vint32m1_t prefix_##r=__riscv_vsub_vv_i32m1(                           \
          __riscv_vmul_vx_i32m1((xr_),(wr_),vl),                             \
          __riscv_vmul_vx_i32m1((xi_),(wi_),vl),vl);                         \
      vint32m1_t prefix_##i=__riscv_vadd_vv_i32m1(                           \
          __riscv_vmul_vx_i32m1((xr_),(wi_),vl),                             \
          __riscv_vmul_vx_i32m1((xi_),(wr_),vl),vl)
#define OAI_RVV_RAD2_U(prefix0_,prefix1_,ar_,ai_,tr_,ti_)                     \
      vint32m1_t prefix0_##r=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(       \
          __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),15,vl);        \
      vint32m1_t prefix0_##i=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(       \
          __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),15,vl);        \
      vint32m1_t prefix1_##r=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(       \
          __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),15,vl);        \
      vint32m1_t prefix1_##i=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(       \
          __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),15,vl)
      OAI_RVV_CMUL_U(t1,x1r,x1i,w128r,w128i);
      OAI_RVV_CMUL_U(t3,x3r,x3i,w128r,w128i);
      OAI_RVV_RAD2_U(p0,p1,x0r,x0i,t1r,t1i);
      OAI_RVV_RAD2_U(p2,p3,x2r,x2i,t3r,t3i);
      OAI_RVV_CMUL_U(q2,p2r,p2i,w256ar,w256ai);
      OAI_RVV_CMUL_U(q3,p3r,p3i,w256br,w256bi);
      OAI_RVV_RAD2_U(y0,y2,p0r,p0i,q2r,q2i);
      OAI_RVV_RAD2_U(y1,y3,p1r,p1i,q3r,q3i);
#undef OAI_RVV_RAD2_U
#undef OAI_RVV_CMUL_U
      __riscv_vsse32_v_i32m1(base+0,stride,OAI_RVV_PACK_COMPLEX(y0r,y0i,vl),vl);
      __riscv_vsse32_v_i32m1(base+64,stride,OAI_RVV_PACK_COMPLEX(y1r,y1i,vl),vl);
      __riscv_vsse32_v_i32m1(base+128,stride,OAI_RVV_PACK_COMPLEX(y2r,y2i,vl),vl);
      __riscv_vsse32_v_i32m1(base+192,stride,OAI_RVV_PACK_COMPLEX(y3r,y3i,vl),vl);
      block += vl;
      }
    }
  }

  /* Fuse stages nine and ten.  Each pass consumes four contiguous 256-point
   * quarters and writes the complete 1024-point result only once. */
  for (unsigned int base = 0; base < 2048; base += 1024) {
    size_t j = 0;
    while (j < 256) {
      const size_t vl = __riscv_vsetvl_e32m1(256 - j);
      int32_t *p = data + base + j;
      vint32m1_t x0p = __riscv_vle32_v_i32m1(p + 0, vl);
      vint32m1_t x1p = __riscv_vle32_v_i32m1(p + 256, vl);
      vint32m1_t x2p = __riscv_vle32_v_i32m1(p + 512, vl);
      vint32m1_t x3p = __riscv_vle32_v_i32m1(p + 768, vl);
      const int32_t *tw32 = (const int32_t *)twiddle;
      vint32m1_t w9p = __riscv_vlse32_v_i32m1(
          tw32 + 4 * j, 4 * (ptrdiff_t)sizeof(int32_t), vl);
      vint32m1_t w10ap = __riscv_vlse32_v_i32m1(
          tw32 + 2 * j, 2 * (ptrdiff_t)sizeof(int32_t), vl);
      vint32m1_t w10bp = __riscv_vlse32_v_i32m1(
          tw32 + 2 * (j + 256), 2 * (ptrdiff_t)sizeof(int32_t), vl);
      OAI_RVV_UNPACK_RE(x0r,x0p,vl); OAI_RVV_UNPACK_IM(x0i,x0p,vl);
      OAI_RVV_UNPACK_RE(x1r,x1p,vl); OAI_RVV_UNPACK_IM(x1i,x1p,vl);
      OAI_RVV_UNPACK_RE(x2r,x2p,vl); OAI_RVV_UNPACK_IM(x2i,x2p,vl);
      OAI_RVV_UNPACK_RE(x3r,x3p,vl); OAI_RVV_UNPACK_IM(x3i,x3p,vl);
      OAI_RVV_UNPACK_RE(w9r,w9p,vl); OAI_RVV_UNPACK_IM(w9i0,w9p,vl);
      OAI_RVV_UNPACK_RE(w10ar,w10ap,vl); OAI_RVV_UNPACK_IM(w10ai0,w10ap,vl);
      OAI_RVV_UNPACK_RE(w10br,w10bp,vl); OAI_RVV_UNPACK_IM(w10bi0,w10bp,vl);
      vint32m1_t w9i=inverse?__riscv_vneg_v_i32m1(w9i0,vl):w9i0;
      vint32m1_t w10ai=inverse?__riscv_vneg_v_i32m1(w10ai0,vl):w10ai0;
      vint32m1_t w10bi=inverse?__riscv_vneg_v_i32m1(w10bi0,vl):w10bi0;
#define OAI_RVV_CMUL_V(prefix_,xr_,xi_,wr_,wi_)                              \
      vint32m1_t prefix_##r=__riscv_vsub_vv_i32m1(                           \
          __riscv_vmul_vv_i32m1((xr_),(wr_),vl),                             \
          __riscv_vmul_vv_i32m1((xi_),(wi_),vl),vl);                         \
      vint32m1_t prefix_##i=__riscv_vadd_vv_i32m1(                           \
          __riscv_vmul_vv_i32m1((xr_),(wi_),vl),                             \
          __riscv_vmul_vv_i32m1((xi_),(wr_),vl),vl)
#define OAI_RVV_RAD2_V(prefix0_,prefix1_,ar_,ai_,tr_,ti_)                    \
      vint32m1_t prefix0_##r=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(      \
          __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),15,vl);       \
      vint32m1_t prefix0_##i=OAI_RVV_FFT32_PACK(__riscv_vadd_vv_i32m1(      \
          __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),15,vl);       \
      vint32m1_t prefix1_##r=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(      \
          __riscv_vmul_vx_i32m1((ar_),INT16_MAX,vl),(tr_),vl),15,vl);       \
      vint32m1_t prefix1_##i=OAI_RVV_FFT32_PACK(__riscv_vsub_vv_i32m1(      \
          __riscv_vmul_vx_i32m1((ai_),INT16_MAX,vl),(ti_),vl),15,vl)
      OAI_RVV_CMUL_V(t1,x1r,x1i,w9r,w9i);
      OAI_RVV_CMUL_V(t3,x3r,x3i,w9r,w9i);
      OAI_RVV_RAD2_V(a0,a1,x0r,x0i,t1r,t1i);
      OAI_RVV_RAD2_V(a2,a3,x2r,x2i,t3r,t3i);
      OAI_RVV_CMUL_V(u2,a2r,a2i,w10ar,w10ai);
      OAI_RVV_CMUL_V(u3,a3r,a3i,w10br,w10bi);
      OAI_RVV_RAD2_V(y0,y2,a0r,a0i,u2r,u2i);
      OAI_RVV_RAD2_V(y1,y3,a1r,a1i,u3r,u3i);
#undef OAI_RVV_RAD2_V
#undef OAI_RVV_CMUL_V
      __riscv_vse32_v_i32m1(p+0,OAI_RVV_PACK_COMPLEX(y0r,y0i,vl),vl);
      __riscv_vse32_v_i32m1(p+256,OAI_RVV_PACK_COMPLEX(y1r,y1i,vl),vl);
      __riscv_vse32_v_i32m1(p+512,OAI_RVV_PACK_COMPLEX(y2r,y2i,vl),vl);
      __riscv_vse32_v_i32m1(p+768,OAI_RVV_PACK_COMPLEX(y3r,y3i,vl),vl);
      j += vl;
    }
  }

  /* Final radix-2 stage and optional 1/sqrt(2) scaling write directly to the
   * caller's output, avoiding a full temporary-array store/load round trip. */
  {
#define OAI_RVV_UNPACK_RE_M2(name_, packed_, vl_)                            \
    vint32m2_t name_ = __riscv_vsra_vx_i32m2(                               \
        __riscv_vsll_vx_i32m2((packed_),16,(vl_)),16,(vl_))
#define OAI_RVV_UNPACK_IM_M2(name_, packed_, vl_)                            \
    vint32m2_t name_ = __riscv_vsra_vx_i32m2((packed_),16,(vl_))
#define OAI_RVV_FFT32_PACK_M2(value_, shift_, vl_)                           \
    __riscv_vmin_vx_i32m2(                                                  \
        __riscv_vmax_vx_i32m2(                                             \
            __riscv_vsra_vx_i32m2(                                         \
                __riscv_vadd_vx_i32m2(                                     \
                    (value_),INT32_C(1)<<((shift_)-1),(vl_)),               \
                (shift_),(vl_)),INT16_MIN,(vl_)),                           \
        INT16_MAX,(vl_))
#define OAI_RVV_PACK_COMPLEX_M2(re_, im_, vl_)                               \
    __riscv_vreinterpret_v_u32m2_i32m2(                                    \
        __riscv_vor_vv_u32m2(                                              \
            __riscv_vand_vx_u32m2(                                        \
                __riscv_vreinterpret_v_i32m2_u32m2((re_)),                 \
                UINT32_C(0xffff),(vl_)),                                   \
            __riscv_vsll_vx_u32m2(                                        \
                __riscv_vreinterpret_v_i32m2_u32m2((im_)),16,(vl_)),       \
            (vl_)))
    size_t j = 0;
    while (j < 1024) {
      const size_t vl = __riscv_vsetvl_e32m2(1024 - j);
      vint32m2_t ap=__riscv_vle32_v_i32m2(data+j,vl);
      vint32m2_t bp=__riscv_vle32_v_i32m2(data+1024+j,vl);
      vint32m2_t tp=__riscv_vle32_v_i32m2((const int32_t *)twiddle+j,vl);
      OAI_RVV_UNPACK_RE_M2(ar,ap,vl); OAI_RVV_UNPACK_IM_M2(ai,ap,vl);
      OAI_RVV_UNPACK_RE_M2(br,bp,vl); OAI_RVV_UNPACK_IM_M2(bi,bp,vl);
      OAI_RVV_UNPACK_RE_M2(wr,tp,vl); OAI_RVV_UNPACK_IM_M2(wi0,tp,vl);
      vint32m2_t wi=inverse?__riscv_vneg_v_i32m2(wi0,vl):wi0;
      vint32m2_t tr=__riscv_vsub_vv_i32m2(
          __riscv_vmul_vv_i32m2(br,wr,vl),__riscv_vmul_vv_i32m2(bi,wi,vl),vl);
      vint32m2_t ti=__riscv_vadd_vv_i32m2(
          __riscv_vmul_vv_i32m2(br,wi,vl),__riscv_vmul_vv_i32m2(bi,wr,vl),vl);
      vint32m2_t aqr=__riscv_vmul_vx_i32m2(ar,INT16_MAX,vl);
      vint32m2_t aqi=__riscv_vmul_vx_i32m2(ai,INT16_MAX,vl);
      vint32m2_t o0r=OAI_RVV_FFT32_PACK_M2(__riscv_vadd_vv_i32m2(aqr,tr,vl),15,vl);
      vint32m2_t o0i=OAI_RVV_FFT32_PACK_M2(__riscv_vadd_vv_i32m2(aqi,ti,vl),15,vl);
      vint32m2_t o1r=OAI_RVV_FFT32_PACK_M2(__riscv_vsub_vv_i32m2(aqr,tr,vl),15,vl);
      vint32m2_t o1i=OAI_RVV_FFT32_PACK_M2(__riscv_vsub_vv_i32m2(aqi,ti,vl),15,vl);
      if (final_scale) {
#define OAI_RVV_FINAL_SCALE(v_)                                               \
        __riscv_vmin_vx_i32m2(__riscv_vmax_vx_i32m2(                         \
            __riscv_vsra_vx_i32m2(__riscv_vadd_vx_i32m2(                    \
                __riscv_vmul_vx_i32m2((v_),23170,vl),16384,vl),15,vl),      \
            INT16_MIN,vl),INT16_MAX,vl)
        o0r=OAI_RVV_FINAL_SCALE(o0r); o0i=OAI_RVV_FINAL_SCALE(o0i);
        o1r=OAI_RVV_FINAL_SCALE(o1r); o1i=OAI_RVV_FINAL_SCALE(o1i);
#undef OAI_RVV_FINAL_SCALE
      }
      __riscv_vse32_v_i32m2((int32_t *)output+j,
          OAI_RVV_PACK_COMPLEX_M2(o0r,o0i,vl),vl);
      vint32m2_t o1p = OAI_RVV_PACK_COMPLEX_M2(o1r,o1i,vl);
      __riscv_vse32_v_i32m2((int32_t *)output+1024+j,o1p,vl);
      /* The cyclic prefix is the tail of the final half.  Duplicate lanes
       * here while o1p is live, avoiding a later load/copy pass. */
      if (prefix_samples != 0 && j + vl > 1024 - prefix_samples) {
        const size_t first = 1024 - prefix_samples;
        const size_t skip = j < first ? first - j : 0;
        const size_t cp_vl = vl - skip;
        vint32m2_t cp = skip ? __riscv_vslidedown_vx_i32m2(o1p,skip,vl) : o1p;
        __riscv_vse32_v_i32m2((int32_t *)output - prefix_samples +
                                  (j + skip - first),
                              cp,cp_vl);
      }
      j += vl;
    }
#undef OAI_RVV_PACK_COMPLEX_M2
#undef OAI_RVV_FFT32_PACK_M2
#undef OAI_RVV_UNPACK_IM_M2
#undef OAI_RVV_UNPACK_RE_M2
  }
#undef OAI_RVV_UNPACK_RE
#undef OAI_RVV_UNPACK_IM
#undef OAI_RVV_FFT32_PACK
#undef OAI_RVV_PACK_COMPLEX
}

/*
 * Fused packed complex Q15 multiply and radix-2 butterfly.  tw_re and tw_im
 * retain OAI's existing madd-compatible pair layout:
 *   re = ar*tw_re[0] + ai*tw_re[1]
 *   im = ar*tw_im[0] + ai*tw_im[1]
 */
static inline void
oai_rvv_bfly2_twiddle_i16(const int16_t *x0, const int16_t *x1,
                          const int16_t *tw_re, const int16_t *tw_im,
                          int16_t *y0, int16_t *y1,
                          size_t complex_count)
{
  const ptrdiff_t complex_stride = 2 * (ptrdiff_t)sizeof(int16_t);
  while (complex_count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(complex_count);
    vint16m1_t x0r = __riscv_vlse16_v_i16m1(x0 + 0, complex_stride, vl);
    vint16m1_t x0i = __riscv_vlse16_v_i16m1(x0 + 1, complex_stride, vl);
    vint16m1_t ar  = __riscv_vlse16_v_i16m1(x1 + 0, complex_stride, vl);
    vint16m1_t ai  = __riscv_vlse16_v_i16m1(x1 + 1, complex_stride, vl);
    vint16m1_t rr  = __riscv_vlse16_v_i16m1(tw_re + 0, complex_stride, vl);
    vint16m1_t ri  = __riscv_vlse16_v_i16m1(tw_re + 1, complex_stride, vl);
    vint16m1_t ir  = __riscv_vlse16_v_i16m1(tw_im + 0, complex_stride, vl);
    vint16m1_t ii  = __riscv_vlse16_v_i16m1(tw_im + 1, complex_stride, vl);

    vint16m1_t tr = oai_rvv_q15_dot2_i16(ar, ai, rr, ri, vl);
    vint16m1_t ti = oai_rvv_q15_dot2_i16(ar, ai, ir, ii, vl);

    vint16m1_t y0r = __riscv_vsadd_vv_i16m1(x0r, tr, vl);
    vint16m1_t y0i = __riscv_vsadd_vv_i16m1(x0i, ti, vl);
    vint16m1_t y1r = __riscv_vssub_vv_i16m1(x0r, tr, vl);
    vint16m1_t y1i = __riscv_vssub_vv_i16m1(x0i, ti, vl);
    __riscv_vsse16_v_i16m1(y0 + 0, complex_stride, y0r, vl);
    __riscv_vsse16_v_i16m1(y0 + 1, complex_stride, y0i, vl);
    __riscv_vsse16_v_i16m1(y1 + 0, complex_stride, y1r, vl);
    __riscv_vsse16_v_i16m1(y1 + 1, complex_stride, y1i, vl);

    const size_t step = 2 * vl;
    x0 += step; x1 += step; tw_re += step; tw_im += step;
    y0 += step; y1 += step;
    complex_count -= vl;
  }
}

/* Fused radix-4 butterfly with three independent packed twiddle pairs. */
static inline void
oai_rvv_bfly4_twiddle_i16(
    const int16_t *x0, const int16_t *x1,
    const int16_t *x2, const int16_t *x3,
    const int16_t *tw1r, const int16_t *tw2r, const int16_t *tw3r,
    const int16_t *tw1i, const int16_t *tw2i, const int16_t *tw3i,
    int16_t *y0, int16_t *y1, int16_t *y2, int16_t *y3,
    size_t complex_count, int inverse)
{
  const ptrdiff_t stride = 2 * (ptrdiff_t)sizeof(int16_t);
  while (complex_count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(complex_count);
    vint16m1_t a0r = __riscv_vlse16_v_i16m1(x0 + 0, stride, vl);
    vint16m1_t a0i = __riscv_vlse16_v_i16m1(x0 + 1, stride, vl);
    vint16m1_t a1r = __riscv_vlse16_v_i16m1(x1 + 0, stride, vl);
    vint16m1_t a1i = __riscv_vlse16_v_i16m1(x1 + 1, stride, vl);
    vint16m1_t a2r = __riscv_vlse16_v_i16m1(x2 + 0, stride, vl);
    vint16m1_t a2i = __riscv_vlse16_v_i16m1(x2 + 1, stride, vl);
    vint16m1_t a3r = __riscv_vlse16_v_i16m1(x3 + 0, stride, vl);
    vint16m1_t a3i = __riscv_vlse16_v_i16m1(x3 + 1, stride, vl);

#define OAI_RVV_LOAD_TWIDDLE_PAIR(prefix, ptr)                                    \
    vint16m1_t prefix##r = __riscv_vlse16_v_i16m1((ptr) + 0, stride, vl);         \
    vint16m1_t prefix##i = __riscv_vlse16_v_i16m1((ptr) + 1, stride, vl)
    OAI_RVV_LOAD_TWIDDLE_PAIR(t1r_, tw1r);
    OAI_RVV_LOAD_TWIDDLE_PAIR(t2r_, tw2r);
    OAI_RVV_LOAD_TWIDDLE_PAIR(t3r_, tw3r);
    OAI_RVV_LOAD_TWIDDLE_PAIR(t1i_, tw1i);
    OAI_RVV_LOAD_TWIDDLE_PAIR(t2i_, tw2i);
    OAI_RVV_LOAD_TWIDDLE_PAIR(t3i_, tw3i);
#undef OAI_RVV_LOAD_TWIDDLE_PAIR

    vint16m1_t b1r = oai_rvv_q15_dot2_i16(a1r, a1i, t1r_r, t1r_i, vl);
    vint16m1_t b1i = oai_rvv_q15_dot2_i16(a1r, a1i, t1i_r, t1i_i, vl);
    vint16m1_t b2r = oai_rvv_q15_dot2_i16(a2r, a2i, t2r_r, t2r_i, vl);
    vint16m1_t b2i = oai_rvv_q15_dot2_i16(a2r, a2i, t2i_r, t2i_i, vl);
    vint16m1_t b3r = oai_rvv_q15_dot2_i16(a3r, a3i, t3r_r, t3r_i, vl);
    vint16m1_t b3i = oai_rvv_q15_dot2_i16(a3r, a3i, t3i_r, t3i_i, vl);

    vint16m1_t ac_r = __riscv_vsadd_vv_i16m1(a0r, b2r, vl);
    vint16m1_t ac_i = __riscv_vsadd_vv_i16m1(a0i, b2i, vl);
    vint16m1_t bd_r = __riscv_vsadd_vv_i16m1(b1r, b3r, vl);
    vint16m1_t bd_i = __riscv_vsadd_vv_i16m1(b1i, b3i, vl);
    vint16m1_t o0r = __riscv_vsadd_vv_i16m1(ac_r, bd_r, vl);
    vint16m1_t o0i = __riscv_vsadd_vv_i16m1(ac_i, bd_i, vl);
    vint16m1_t o2r = __riscv_vssub_vv_i16m1(ac_r, bd_r, vl);
    vint16m1_t o2i = __riscv_vssub_vv_i16m1(ac_i, bd_i, vl);

    vint16m1_t diff_r = __riscv_vssub_vv_i16m1(a0r, b2r, vl);
    vint16m1_t diff_i = __riscv_vssub_vv_i16m1(a0i, b2i, vl);
    /* -j*b1 - (-j*b3) = (b1i-b3i, b3r-b1r). */
    vint16m1_t jdiff_r = __riscv_vssub_vv_i16m1(b1i, b3i, vl);
    vint16m1_t jdiff_i = __riscv_vssub_vv_i16m1(b3r, b1r, vl);
    vint16m1_t f1r = __riscv_vsadd_vv_i16m1(diff_r, jdiff_r, vl);
    vint16m1_t f1i = __riscv_vsadd_vv_i16m1(diff_i, jdiff_i, vl);
    vint16m1_t f3r = __riscv_vssub_vv_i16m1(diff_r, jdiff_r, vl);
    vint16m1_t f3i = __riscv_vssub_vv_i16m1(diff_i, jdiff_i, vl);

#define OAI_RVV_STORE_COMPLEX(ptr, re, im)                         \
    __riscv_vsse16_v_i16m1((ptr) + 0, stride, (re), vl);           \
    __riscv_vsse16_v_i16m1((ptr) + 1, stride, (im), vl)
    OAI_RVV_STORE_COMPLEX(y0, o0r, o0i);
    OAI_RVV_STORE_COMPLEX(y2, o2r, o2i);
    if (inverse) {
      OAI_RVV_STORE_COMPLEX(y3, f1r, f1i);
      OAI_RVV_STORE_COMPLEX(y1, f3r, f3i);
    } else {
      OAI_RVV_STORE_COMPLEX(y1, f1r, f1i);
      OAI_RVV_STORE_COMPLEX(y3, f3r, f3i);
    }
#undef OAI_RVV_STORE_COMPLEX

    const size_t step = 2 * vl;
    x0 += step; x1 += step; x2 += step; x3 += step;
    tw1r += step; tw2r += step; tw3r += step;
    tw1i += step; tw2i += step; tw3i += step;
    y0 += step; y1 += step; y2 += step; y3 += step;
    complex_count -= vl;
  }
}

static inline vint16m1_t
oai_rvv_q15_acc4_i16(vint16m1_t a0, vint16m1_t a1,
                     vint16m1_t b0, vint16m1_t b1,
                     vint16m1_t c0, vint16m1_t c1,
                     vint16m1_t d0, vint16m1_t d1, size_t vl)
{
  vint32m2_t sum = __riscv_vwmul_vv_i32m2(a0, b0, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, a1, b1, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, c0, d0, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, c1, d1, vl);
  sum = __riscv_vsra_vx_i32m2(sum, 15, vl);
  sum = __riscv_vmax_vx_i32m2(sum, INT16_MIN, vl);
  sum = __riscv_vmin_vx_i32m2(sum, INT16_MAX, vl);
  return __riscv_vnsra_wx_i16m1(sum, 0, vl);
}

static inline vint16m1_t
oai_rvv_q15_acc8_i16(vint16m1_t a0, vint16m1_t a1,
                     vint16m1_t b0, vint16m1_t b1,
                     vint16m1_t c0, vint16m1_t c1,
                     vint16m1_t d0, vint16m1_t d1,
                     vint16m1_t e0, vint16m1_t e1,
                     vint16m1_t f0, vint16m1_t f1,
                     vint16m1_t g0, vint16m1_t g1,
                     vint16m1_t h0, vint16m1_t h1, size_t vl)
{
  vint32m2_t sum = __riscv_vwmul_vv_i32m2(a0, b0, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, a1, b1, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, c0, d0, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, c1, d1, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, e0, f0, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, e1, f1, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, g0, h0, vl);
  sum = __riscv_vwmacc_vv_i32m2(sum, g1, h1, vl);
  sum = __riscv_vsra_vx_i32m2(sum, 15, vl);
  sum = __riscv_vmax_vx_i32m2(sum, INT16_MIN, vl);
  sum = __riscv_vmin_vx_i32m2(sum, INT16_MAX, vl);
  return __riscv_vnsra_wx_i16m1(sum, 0, vl);
}

/* Scalable forward radix-5 butterfly.  If tw1 is NULL the four input
 * twiddles are unity, which covers bfly5_tw1 without a second kernel. */
static inline void
oai_rvv_bfly5_i16(const int16_t *x0, const int16_t *x1,
                  const int16_t *x2, const int16_t *x3,
                  const int16_t *x4,
                  const int16_t *tw1, const int16_t *tw2,
                  const int16_t *tw3, const int16_t *tw4,
                  const int16_t *w15, const int16_t *w25,
                  const int16_t *w35, const int16_t *w45,
                  int16_t *y0, int16_t *y1, int16_t *y2,
                  int16_t *y3, int16_t *y4, size_t complex_count)
{
  const ptrdiff_t stride = 2 * (ptrdiff_t)sizeof(int16_t);
  while (complex_count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(complex_count);
#define OAI_RVV_LOAD_C5(prefix, ptr)                                      \
    vint16m1_t prefix##r = __riscv_vlse16_v_i16m1((ptr), stride, vl);     \
    vint16m1_t prefix##i = __riscv_vlse16_v_i16m1((ptr) + 1, stride, vl)
    OAI_RVV_LOAD_C5(a0, x0); OAI_RVV_LOAD_C5(a1, x1);
    OAI_RVV_LOAD_C5(a2, x2); OAI_RVV_LOAD_C5(a3, x3);
    OAI_RVV_LOAD_C5(a4, x4);
    OAI_RVV_LOAD_C5(c1, w15); OAI_RVV_LOAD_C5(c2, w25);
    OAI_RVV_LOAD_C5(c3, w35); OAI_RVV_LOAD_C5(c4, w45);

    vint16m1_t b1r = a1r, b1i = a1i, b2r = a2r, b2i = a2i;
    vint16m1_t b3r = a3r, b3i = a3i, b4r = a4r, b4i = a4i;
    if (tw1 != NULL) {
      OAI_RVV_LOAD_C5(t1, tw1); OAI_RVV_LOAD_C5(t2, tw2);
      OAI_RVV_LOAD_C5(t3, tw3); OAI_RVV_LOAD_C5(t4, tw4);
      b1r = oai_rvv_q15_dot2_i16(a1r, a1i, t1r, __riscv_vneg_v_i16m1(t1i, vl), vl);
      b1i = oai_rvv_q15_dot2_i16(a1r, a1i, t1i, t1r, vl);
      b2r = oai_rvv_q15_dot2_i16(a2r, a2i, t2r, __riscv_vneg_v_i16m1(t2i, vl), vl);
      b2i = oai_rvv_q15_dot2_i16(a2r, a2i, t2i, t2r, vl);
      b3r = oai_rvv_q15_dot2_i16(a3r, a3i, t3r, __riscv_vneg_v_i16m1(t3i, vl), vl);
      b3i = oai_rvv_q15_dot2_i16(a3r, a3i, t3i, t3r, vl);
      b4r = oai_rvv_q15_dot2_i16(a4r, a4i, t4r, __riscv_vneg_v_i16m1(t4i, vl), vl);
      b4i = oai_rvv_q15_dot2_i16(a4r, a4i, t4i, t4r, vl);
    }
#undef OAI_RVV_LOAD_C5

    vint16m1_t s34r = __riscv_vsadd_vv_i16m1(b3r, b4r, vl);
    vint16m1_t s34i = __riscv_vsadd_vv_i16m1(b3i, b4i, vl);
    vint16m1_t s234r = __riscv_vsadd_vv_i16m1(b2r, s34r, vl);
    vint16m1_t s234i = __riscv_vsadd_vv_i16m1(b2i, s34i, vl);
    vint16m1_t s1234r = __riscv_vsadd_vv_i16m1(b1r, s234r, vl);
    vint16m1_t s1234i = __riscv_vsadd_vv_i16m1(b1i, s234i, vl);
    vint16m1_t o0r = __riscv_vsadd_vv_i16m1(a0r, s1234r, vl);
    vint16m1_t o0i = __riscv_vsadd_vv_i16m1(a0i, s1234i, vl);

#define OAI_RVV_RADIX5_OUT(or_, oi_, ar, ai, br, bi, cr, ci, dr, di)       \
    vint16m1_t n##or_##ai = __riscv_vneg_v_i16m1((ai), vl);               \
    vint16m1_t n##or_##bi = __riscv_vneg_v_i16m1((bi), vl);               \
    vint16m1_t n##or_##ci = __riscv_vneg_v_i16m1((ci), vl);               \
    vint16m1_t n##or_##di = __riscv_vneg_v_i16m1((di), vl);               \
    vint16m1_t or_ = oai_rvv_q15_acc8_i16(                                 \
        b1r, (ar), b1i, n##or_##ai, b2r, (br), b2i, n##or_##bi,           \
        b3r, (cr), b3i, n##or_##ci, b4r, (dr), b4i, n##or_##di, vl);      \
    vint16m1_t oi_ = oai_rvv_q15_acc8_i16(                                 \
        b1r, (ai), b1i, (ar), b2r, (bi), b2i, (br),                       \
        b3r, (ci), b3i, (cr), b4r, (di), b4i, (dr), vl)
    OAI_RVV_RADIX5_OUT(q1r, q1i, c1r,c1i, c2r,c2i, c3r,c3i, c4r,c4i);
    OAI_RVV_RADIX5_OUT(q2r, q2i, c2r,c2i, c4r,c4i, c1r,c1i, c3r,c3i);
    OAI_RVV_RADIX5_OUT(q3r, q3i, c3r,c3i, c1r,c1i, c4r,c4i, c2r,c2i);
    OAI_RVV_RADIX5_OUT(q4r, q4i, c4r,c4i, c3r,c3i, c2r,c2i, c1r,c1i);
#undef OAI_RVV_RADIX5_OUT

#define OAI_RVV_STORE_C5(ptr, re, im)                        \
    __riscv_vsse16_v_i16m1((ptr), stride, (re), vl);         \
    __riscv_vsse16_v_i16m1((ptr) + 1, stride, (im), vl)
    OAI_RVV_STORE_C5(y0, o0r, o0i);
    OAI_RVV_STORE_C5(y1, __riscv_vsadd_vv_i16m1(a0r,q1r,vl), __riscv_vsadd_vv_i16m1(a0i,q1i,vl));
    OAI_RVV_STORE_C5(y2, __riscv_vsadd_vv_i16m1(a0r,q2r,vl), __riscv_vsadd_vv_i16m1(a0i,q2i,vl));
    OAI_RVV_STORE_C5(y3, __riscv_vsadd_vv_i16m1(a0r,q3r,vl), __riscv_vsadd_vv_i16m1(a0i,q3i,vl));
    OAI_RVV_STORE_C5(y4, __riscv_vsadd_vv_i16m1(a0r,q4r,vl), __riscv_vsadd_vv_i16m1(a0i,q4i,vl));
#undef OAI_RVV_STORE_C5
    const size_t step = 2 * vl;
    x0 += step; x1 += step; x2 += step; x3 += step; x4 += step;
    if (tw1 != NULL) { tw1 += step; tw2 += step; tw3 += step; tw4 += step; }
    w15 += step; w25 += step; w35 += step; w45 += step;
    y0 += step; y1 += step; y2 += step; y3 += step; y4 += step;
    complex_count -= vl;
  }
}

/* Three-way butterfly without input twiddles (W13/W23 coefficient pairs). */
static inline void
oai_rvv_bfly3_tw1_i16(const int16_t *x0, const int16_t *x1,
                      const int16_t *x2,
                      const int16_t *w13, const int16_t *w23,
                      int16_t *y0, int16_t *y1, int16_t *y2,
                      size_t complex_count)
{
  const ptrdiff_t stride = 2 * (ptrdiff_t)sizeof(int16_t);
  while (complex_count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(complex_count);
#define OAI_RVV_LOAD_COMPLEX(prefix, ptr)                                      \
    vint16m1_t prefix##r = __riscv_vlse16_v_i16m1((ptr) + 0, stride, vl);      \
    vint16m1_t prefix##i = __riscv_vlse16_v_i16m1((ptr) + 1, stride, vl)
    OAI_RVV_LOAD_COMPLEX(a0, x0);
    OAI_RVV_LOAD_COMPLEX(a1, x1);
    OAI_RVV_LOAD_COMPLEX(a2, x2);
    OAI_RVV_LOAD_COMPLEX(c13, w13);
    OAI_RVV_LOAD_COMPLEX(c23, w23);
#undef OAI_RVV_LOAD_COMPLEX

    vint16m1_t x12r = __riscv_vsadd_vv_i16m1(a1r, a2r, vl);
    vint16m1_t x12i = __riscv_vsadd_vv_i16m1(a1i, a2i, vl);
    vint16m1_t o0r = __riscv_vsadd_vv_i16m1(a0r, x12r, vl);
    vint16m1_t o0i = __riscv_vsadd_vv_i16m1(a0i, x12i, vl);

    /* (a1*c13 + a2*c23), with both products accumulated before Q15 shift. */
    vint16m1_t t1r = oai_rvv_q15_acc4_i16(
        a1r, a1i, c13r, __riscv_vneg_v_i16m1(c13i, vl),
        a2r, a2i, c23r, __riscv_vneg_v_i16m1(c23i, vl), vl);
    vint16m1_t t1i = oai_rvv_q15_acc4_i16(
        a1r, a1i, c13i, c13r, a2r, a2i, c23i, c23r, vl);
    vint16m1_t t2r = oai_rvv_q15_acc4_i16(
        a1r, a1i, c23r, __riscv_vneg_v_i16m1(c23i, vl),
        a2r, a2i, c13r, __riscv_vneg_v_i16m1(c13i, vl), vl);
    vint16m1_t t2i = oai_rvv_q15_acc4_i16(
        a1r, a1i, c23i, c23r, a2r, a2i, c13i, c13r, vl);
    vint16m1_t o1r = __riscv_vsadd_vv_i16m1(a0r, t1r, vl);
    vint16m1_t o1i = __riscv_vsadd_vv_i16m1(a0i, t1i, vl);
    vint16m1_t o2r = __riscv_vsadd_vv_i16m1(a0r, t2r, vl);
    vint16m1_t o2i = __riscv_vsadd_vv_i16m1(a0i, t2i, vl);

#define OAI_RVV_STORE_COMPLEX3(ptr, re, im)                       \
    __riscv_vsse16_v_i16m1((ptr) + 0, stride, (re), vl);          \
    __riscv_vsse16_v_i16m1((ptr) + 1, stride, (im), vl)
    OAI_RVV_STORE_COMPLEX3(y0, o0r, o0i);
    OAI_RVV_STORE_COMPLEX3(y1, o1r, o1i);
    OAI_RVV_STORE_COMPLEX3(y2, o2r, o2i);
#undef OAI_RVV_STORE_COMPLEX3
    const size_t step = 2 * vl;
    x0 += step; x1 += step; x2 += step;
    w13 += step; w23 += step;
    y0 += step; y1 += step; y2 += step;
    complex_count -= vl;
  }
}

static inline void
oai_rvv_bfly3_twiddle_i16(const int16_t *x0, const int16_t *x1,
                          const int16_t *x2,
                          const int16_t *tw1, const int16_t *tw2,
                          const int16_t *w13, const int16_t *w23,
                          int16_t *y0, int16_t *y1, int16_t *y2,
                          size_t complex_count)
{
  const ptrdiff_t stride = 2 * (ptrdiff_t)sizeof(int16_t);
  while (complex_count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(complex_count);
#define OAI_RVV_LOAD_COMPLEX_TW3(prefix, ptr)                                  \
    vint16m1_t prefix##r = __riscv_vlse16_v_i16m1((ptr) + 0, stride, vl);      \
    vint16m1_t prefix##i = __riscv_vlse16_v_i16m1((ptr) + 1, stride, vl)
    OAI_RVV_LOAD_COMPLEX_TW3(a0, x0);
    OAI_RVV_LOAD_COMPLEX_TW3(a1, x1);
    OAI_RVV_LOAD_COMPLEX_TW3(a2, x2);
    OAI_RVV_LOAD_COMPLEX_TW3(t1, tw1);
    OAI_RVV_LOAD_COMPLEX_TW3(t2, tw2);
    OAI_RVV_LOAD_COMPLEX_TW3(c13, w13);
    OAI_RVV_LOAD_COMPLEX_TW3(c23, w23);
#undef OAI_RVV_LOAD_COMPLEX_TW3

    vint16m1_t n_t1i = __riscv_vneg_v_i16m1(t1i, vl);
    vint16m1_t n_t2i = __riscv_vneg_v_i16m1(t2i, vl);
    vint16m1_t b1r = oai_rvv_q15_dot2_i16(a1r, a1i, t1r, n_t1i, vl);
    vint16m1_t b1i = oai_rvv_q15_dot2_i16(a1r, a1i, t1i, t1r, vl);
    vint16m1_t b2r = oai_rvv_q15_dot2_i16(a2r, a2i, t2r, n_t2i, vl);
    vint16m1_t b2i = oai_rvv_q15_dot2_i16(a2r, a2i, t2i, t2r, vl);

    vint16m1_t b12r = __riscv_vsadd_vv_i16m1(b1r, b2r, vl);
    vint16m1_t b12i = __riscv_vsadd_vv_i16m1(b1i, b2i, vl);
    vint16m1_t o0r = __riscv_vsadd_vv_i16m1(a0r, b12r, vl);
    vint16m1_t o0i = __riscv_vsadd_vv_i16m1(a0i, b12i, vl);

    vint16m1_t n_c13i = __riscv_vneg_v_i16m1(c13i, vl);
    vint16m1_t n_c23i = __riscv_vneg_v_i16m1(c23i, vl);
    vint16m1_t q1r = oai_rvv_q15_acc4_i16(
        b1r, b1i, c13r, n_c13i, b2r, b2i, c23r, n_c23i, vl);
    vint16m1_t q1i = oai_rvv_q15_acc4_i16(
        b1r, b1i, c13i, c13r, b2r, b2i, c23i, c23r, vl);
    vint16m1_t q2r = oai_rvv_q15_acc4_i16(
        b1r, b1i, c23r, n_c23i, b2r, b2i, c13r, n_c13i, vl);
    vint16m1_t q2i = oai_rvv_q15_acc4_i16(
        b1r, b1i, c23i, c23r, b2r, b2i, c13i, c13r, vl);
    vint16m1_t o1r = __riscv_vsadd_vv_i16m1(a0r, q1r, vl);
    vint16m1_t o1i = __riscv_vsadd_vv_i16m1(a0i, q1i, vl);
    vint16m1_t o2r = __riscv_vsadd_vv_i16m1(a0r, q2r, vl);
    vint16m1_t o2i = __riscv_vsadd_vv_i16m1(a0i, q2i, vl);

#define OAI_RVV_STORE_COMPLEX_TW3(ptr, re, im)                    \
    __riscv_vsse16_v_i16m1((ptr) + 0, stride, (re), vl);          \
    __riscv_vsse16_v_i16m1((ptr) + 1, stride, (im), vl)
    OAI_RVV_STORE_COMPLEX_TW3(y0, o0r, o0i);
    OAI_RVV_STORE_COMPLEX_TW3(y1, o1r, o1i);
    OAI_RVV_STORE_COMPLEX_TW3(y2, o2r, o2i);
#undef OAI_RVV_STORE_COMPLEX_TW3
    const size_t step = 2 * vl;
    x0 += step; x1 += step; x2 += step; tw1 += step; tw2 += step;
    w13 += step; w23 += step; y0 += step; y1 += step; y2 += step;
    complex_count -= vl;
  }
}

/* Inverse radix-3 butterfly matching OAI's conjugate-multiply semantics. */
static inline void
oai_rvv_ibfly3_twiddle_i16(const int16_t *x0, const int16_t *x1,
                           const int16_t *x2,
                           const int16_t *tw1, const int16_t *tw2,
                           const int16_t *w13, const int16_t *w23,
                           int16_t *y0, int16_t *y1, int16_t *y2,
                           size_t complex_count)
{
  const ptrdiff_t stride = 2 * (ptrdiff_t)sizeof(int16_t);
  while (complex_count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(complex_count);
#define OAI_RVV_LOAD_COMPLEX_ITW3(prefix, ptr)                                 \
    vint16m1_t prefix##r = __riscv_vlse16_v_i16m1((ptr) + 0, stride, vl);      \
    vint16m1_t prefix##i = __riscv_vlse16_v_i16m1((ptr) + 1, stride, vl)
    OAI_RVV_LOAD_COMPLEX_ITW3(a0, x0);
    OAI_RVV_LOAD_COMPLEX_ITW3(a1, x1);
    OAI_RVV_LOAD_COMPLEX_ITW3(a2, x2);
    OAI_RVV_LOAD_COMPLEX_ITW3(t1, tw1);
    OAI_RVV_LOAD_COMPLEX_ITW3(t2, tw2);
    OAI_RVV_LOAD_COMPLEX_ITW3(c13, w13);
    OAI_RVV_LOAD_COMPLEX_ITW3(c23, w23);
#undef OAI_RVV_LOAD_COMPLEX_ITW3

    vint16m1_t nt1i = __riscv_vneg_v_i16m1(t1i, vl);
    vint16m1_t nt2i = __riscv_vneg_v_i16m1(t2i, vl);
    vint16m1_t b1r = oai_rvv_q15_dot2_i16(a1r, a1i, t1r, t1i, vl);
    vint16m1_t b1i = oai_rvv_q15_dot2_i16(a1r, a1i, nt1i, t1r, vl);
    vint16m1_t b2r = oai_rvv_q15_dot2_i16(a2r, a2i, t2r, t2i, vl);
    vint16m1_t b2i = oai_rvv_q15_dot2_i16(a2r, a2i, nt2i, t2r, vl);
    vint16m1_t o0r = __riscv_vsadd_vv_i16m1(
        a0r, __riscv_vsadd_vv_i16m1(b1r, b2r, vl), vl);
    vint16m1_t o0i = __riscv_vsadd_vv_i16m1(
        a0i, __riscv_vsadd_vv_i16m1(b1i, b2i, vl), vl);

    vint16m1_t nc13i = __riscv_vneg_v_i16m1(c13i, vl);
    vint16m1_t nc23i = __riscv_vneg_v_i16m1(c23i, vl);
    vint16m1_t q1r = oai_rvv_q15_acc4_i16(
        b1r, b1i, c13r, c13i, b2r, b2i, c23r, c23i, vl);
    vint16m1_t q1i = oai_rvv_q15_acc4_i16(
        b1r, b1i, nc13i, c13r, b2r, b2i, nc23i, c23r, vl);
    vint16m1_t q2r = oai_rvv_q15_acc4_i16(
        b1r, b1i, c23r, c23i, b2r, b2i, c13r, c13i, vl);
    vint16m1_t q2i = oai_rvv_q15_acc4_i16(
        b1r, b1i, nc23i, c23r, b2r, b2i, nc13i, c13r, vl);
    vint16m1_t o1r = __riscv_vsadd_vv_i16m1(a0r, q1r, vl);
    vint16m1_t o1i = __riscv_vsadd_vv_i16m1(a0i, q1i, vl);
    vint16m1_t o2r = __riscv_vsadd_vv_i16m1(a0r, q2r, vl);
    vint16m1_t o2i = __riscv_vsadd_vv_i16m1(a0i, q2i, vl);

#define OAI_RVV_STORE_COMPLEX_ITW3(ptr, re, im)                   \
    __riscv_vsse16_v_i16m1((ptr) + 0, stride, (re), vl);          \
    __riscv_vsse16_v_i16m1((ptr) + 1, stride, (im), vl)
    OAI_RVV_STORE_COMPLEX_ITW3(y0, o0r, o0i);
    OAI_RVV_STORE_COMPLEX_ITW3(y1, o1r, o1i);
    OAI_RVV_STORE_COMPLEX_ITW3(y2, o2r, o2i);
#undef OAI_RVV_STORE_COMPLEX_ITW3
    const size_t step = 2 * vl;
    x0 += step; x1 += step; x2 += step; tw1 += step; tw2 += step;
    w13 += step; w23 += step; y0 += step; y1 += step; y2 += step;
    complex_count -= vl;
  }
}

/* Four-way complex butterfly with unity twiddles. */
static inline void
oai_rvv_bfly4_tw1_i16(const int16_t *x0, const int16_t *x1,
                      const int16_t *x2, const int16_t *x3,
                      int16_t *y0, int16_t *y1,
                      int16_t *y2, int16_t *y3,
                      size_t count, int inverse)
{
  while (count != 0) {
    const size_t vl = __riscv_vsetvl_e16m1(count);
    vint16m1_t a = __riscv_vle16_v_i16m1(x0, vl);
    vint16m1_t b = __riscv_vle16_v_i16m1(x1, vl);
    vint16m1_t c = __riscv_vle16_v_i16m1(x2, vl);
    vint16m1_t d = __riscv_vle16_v_i16m1(x3, vl);

    vint16m1_t ac_sum = __riscv_vsadd_vv_i16m1(a, c, vl);
    vint16m1_t bd_sum = __riscv_vsadd_vv_i16m1(b, d, vl);
    vint16m1_t out0 = __riscv_vsadd_vv_i16m1(ac_sum, bd_sum, vl);
    vint16m1_t out2 = __riscv_vssub_vv_i16m1(ac_sum, bd_sum, vl);

    vint16m1_t jb = oai_rvv_rotate_minus_j_i16(b, vl);
    vint16m1_t jd = oai_rvv_rotate_minus_j_i16(d, vl);
    vint16m1_t ac_diff = __riscv_vssub_vv_i16m1(a, c, vl);
    vint16m1_t jdiff = __riscv_vssub_vv_i16m1(jb, jd, vl);
    vint16m1_t forward1 = __riscv_vsadd_vv_i16m1(ac_diff, jdiff, vl);
    vint16m1_t forward3 = __riscv_vssub_vv_i16m1(ac_diff, jdiff, vl);

    __riscv_vse16_v_i16m1(y0, out0, vl);
    __riscv_vse16_v_i16m1(y2, out2, vl);
    __riscv_vse16_v_i16m1(inverse ? y3 : y1, forward1, vl);
    __riscv_vse16_v_i16m1(inverse ? y1 : y3, forward3, vl);

    x0 += vl; x1 += vl; x2 += vl; x3 += vl;
    y0 += vl; y1 += vl; y2 += vl; y3 += vl;
    count -= vl;
  }
}

static inline void
oai_rvv_transpose_4x8_i32(const int32_t *src, int32_t *dst,
                          ptrdiff_t dst_vector_stride)
{
  size_t done = 0;
  while (done < 8) {
    const size_t vl = __riscv_vsetvl_e32m1(8 - done);
    vuint32m1_t lane = __riscv_vid_v_u32m1(vl);
    lane = __riscv_vadd_vx_u32m1(lane, (uint32_t)done, vl);
    /* Source byte offsets: 0,16,32,... (one element from each 4-wide row). */
    vuint32m1_t byte_offset = __riscv_vsll_vx_u32m1(lane, 4, vl);
    vint32m1_t c0 = __riscv_vluxei32_v_i32m1(src + 0, byte_offset, vl);
    vint32m1_t c1 = __riscv_vluxei32_v_i32m1(src + 1, byte_offset, vl);
    vint32m1_t c2 = __riscv_vluxei32_v_i32m1(src + 2, byte_offset, vl);
    vint32m1_t c3 = __riscv_vluxei32_v_i32m1(src + 3, byte_offset, vl);
    __riscv_vse32_v_i32m1(dst + 0 * dst_vector_stride * 8 + done, c0, vl);
    __riscv_vse32_v_i32m1(dst + 1 * dst_vector_stride * 8 + done, c1, vl);
    __riscv_vse32_v_i32m1(dst + 2 * dst_vector_stride * 8 + done, c2, vl);
    __riscv_vse32_v_i32m1(dst + 3 * dst_vector_stride * 8 + done, c3, vl);
    done += vl;
  }
}

static inline void
oai_rvv_deinterleave_2x8_i32(const int32_t *src, int32_t *dst,
                             ptrdiff_t dst_vector_stride)
{
  size_t done = 0;
  while (done < 8) {
    const size_t vl = __riscv_vsetvl_e32m1(8 - done);
    vuint32m1_t lane = __riscv_vid_v_u32m1(vl);
    lane = __riscv_vadd_vx_u32m1(lane, (uint32_t)done, vl);
    /* Source byte offsets: 0,8,16,... (even/odd int32 elements). */
    vuint32m1_t byte_offset = __riscv_vsll_vx_u32m1(lane, 3, vl);
    vint32m1_t even = __riscv_vluxei32_v_i32m1(src + 0, byte_offset, vl);
    vint32m1_t odd  = __riscv_vluxei32_v_i32m1(src + 1, byte_offset, vl);
    __riscv_vse32_v_i32m1(dst + done, even, vl);
    __riscv_vse32_v_i32m1(dst + dst_vector_stride * 8 + done, odd, vl);
    done += vl;
  }
}

#endif /* __riscv_vector */
#endif /* OAI_DFTS_RVV_H */
