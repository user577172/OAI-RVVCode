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
