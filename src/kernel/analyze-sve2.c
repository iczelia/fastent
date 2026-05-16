/*  fastent: AArch64 SVE2 analyse variants.  Self-contained because
    analyze-impl.h assumes a compile-time fixed vector stride.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "analyze.h"

#if defined(__ARM_BIG_ENDIAN) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "fastent SVE2 variant is not supported on big-endian ARM"
#endif

#include <arm_sve.h>
#include <string.h>

static inline u8 sve2_fold_byte(u8 b) {
  unsigned c = b;
  int is_ascii_upper = (c - 'A')   < 26u;
  int is_lat_upper   = ((c - 0xC0u) < 31u) && (c != 0xD7u);
  if (is_ascii_upper || is_lat_upper) return (u8)(c + 0x20u);
  return b;
}

static inline svuint8_t sve2_fold_vec(svbool_t pg, svuint8_t v) {
  svuint8_t v_amin  = svdup_n_u8('A');
  svuint8_t v_zmax  = svdup_n_u8('Z');
  svuint8_t v_c0min = svdup_n_u8(0xC0);
  svuint8_t v_demax = svdup_n_u8(0xDE);
  svuint8_t v_d7    = svdup_n_u8(0xD7);
  svuint8_t v_0x20  = svdup_n_u8(0x20);
  svbool_t  m_ascii = svand_b_z(pg,
                        svcmpge_u8(pg, v, v_amin),
                        svcmple_u8(pg, v, v_zmax));
  svbool_t  m_latin = svand_b_z(pg,
                        svand_b_z(pg,
                          svcmpge_u8(pg, v, v_c0min),
                          svcmple_u8(pg, v, v_demax)),
                        svcmpne_u8(pg, v, v_d7));
  svbool_t  m_fold  = svorr_b_z(pg, m_ascii, m_latin);
  return svadd_u8_m(m_fold, v, v_0x20);
}

#define SVE2_MC_COMMIT(_st)                                                \
  do {                                                                     \
    u32 _x = ((u32) (_st)->mc_buf[0] << 16)                                \
           | ((u32) (_st)->mc_buf[1] <<  8)                                \
           |  (u32) (_st)->mc_buf[2];                                      \
    u32 _y = ((u32) (_st)->mc_buf[3] << 16)                                \
           | ((u32) (_st)->mc_buf[4] <<  8)                                \
           |  (u32) (_st)->mc_buf[5];                                      \
    u64 _d = (u64) _x * (u64) _x + (u64) _y * (u64) _y;                    \
    (_st)->mc_count++;                                                     \
    if (_d <= FASTENT_INCIRC) (_st)->mc_inside++;                          \
    (_st)->mc_pos = 0;                                                     \
  } while (0)

static void analyze_sve2_byte_impl(fastent_chunk_state * st,
                                   const u8 * buf, sz len, int fold) {
  if (len == 0) return;
  const u64 stride = svcntb();
  svbool_t pg8  = svptrue_b8();
  svbool_t pg32 = svptrue_b32();
  int will_sve2 = ((u64) len >= stride + 1) ? 1 : 0;
  sz i = 0;

  if (will_sve2) {
    u8 b0 = fold ? sve2_fold_byte(buf[0]) : buf[0];
    if (st->have_carry) {
      st->cross_product += (i64) st->carry_byte * (i64) b0;
    } else {
      st->first_byte = b0;
      st->have_first = 1;
    }

    while (i + stride + 1 <= (u64) len) {
      svuint8_t v      = svld1_u8(pg8, buf + i);
      svuint8_t v_next = svld1_u8(pg8, buf + i + 1);
      if (fold) {
        v      = sve2_fold_vec(pg8, v);
        v_next = sve2_fold_vec(pg8, v_next);
      }
      /*  svdot_u32 sums the stride byte-products into u32 lanes;
          v_next loaded from buf+i+1 makes the across-chunk pair
          (i+stride-1, i+stride) included here.  */
      svuint32_t prod = svdot_u32(svdup_n_u32(0), v, v_next);
      st->cross_product += (i64) svaddv_u32(pg32, prod);

      for (u64 k = 0; k < stride; k++) {
        u8 b = fold ? sve2_fold_byte(buf[i + k]) : buf[i + k];
        st->bank[(unsigned)(i + k) & (FASTENT_BANKS - 1)][b]++;
        st->mc_buf[st->mc_pos++] = b;
        if (st->mc_pos == 6) SVE2_MC_COMMIT(st);
      }
      st->total_bytes += stride;
      u8 last_b = fold ? sve2_fold_byte(buf[i + stride - 1])
                       : buf[i + stride - 1];
      st->carry_byte = last_b;
      st->last_byte  = last_b;
      st->have_carry = 1;
      i += stride;
    }

    /*  Un-count the cross-boundary pair; the scalar tail re-adds it.  */
    if (i > 0 && i < (u64) len) {
      u8 prev = fold ? sve2_fold_byte(buf[i - 1]) : buf[i - 1];
      u8 cur  = fold ? sve2_fold_byte(buf[i])     : buf[i];
      st->cross_product -= (i64) prev * (i64) cur;
    }
  }

  for (; i < (u64) len; i++) {
    u8 b = fold ? sve2_fold_byte(buf[i]) : buf[i];
    if (st->have_carry) {
      st->cross_product += (i64) st->carry_byte * (i64) b;
    } else {
      st->first_byte = b;
      st->have_first = 1;
    }
    st->carry_byte = b;
    st->have_carry = 1;
    st->last_byte  = b;
    st->bank[(unsigned) i & (FASTENT_BANKS - 1)][b]++;
    st->mc_buf[st->mc_pos++] = b;
    if (st->mc_pos == 6) SVE2_MC_COMMIT(st);
    st->total_bytes++;
  }
}

void analyze_sve2(fastent_chunk_state * st, const u8 * buf, sz len) {
  analyze_sve2_byte_impl(st, buf, len, 0);
}

void analyze_fold_sve2(fastent_chunk_state * st, const u8 * buf, sz len) {
  analyze_sve2_byte_impl(st, buf, len, 1);
}

static inline void sve2_consume_bit_byte(fastent_chunk_state * st, u8 b) {
  const unsigned ones_in_byte = FASTENT_POPCOUNT32(b);
  st->bit_hist[1] += ones_in_byte;
  st->bit_hist[0] += 8u - ones_in_byte;
  const unsigned within = FASTENT_POPCOUNT32(b & (b >> 1));
  st->cross_product += (i64) within;
  if (st->have_carry) {
    const unsigned prev_lsb = (unsigned)(st->carry_byte & 1u);
    const unsigned curr_msb = (unsigned)((b >> 7) & 1u);
    st->cross_product += (i64)(prev_lsb & curr_msb);
  } else {
    st->first_byte = (u8)((b >> 7) & 1u);
    st->have_first = 1;
  }
  st->carry_byte = (u8)(b & 1u);
  st->last_byte  = (u8)(b & 1u);
  st->mc_buf[st->mc_pos++] = b;
  if (st->mc_pos == 6) SVE2_MC_COMMIT(st);
  st->total_bytes += 8;
}

static void analyze_bits_sve2_impl(fastent_chunk_state * st,
                                   const u8 * buf, sz len, int fold) {
  if (len == 0) return;
  const u64 stride = svcntb();
  svbool_t pg = svptrue_b8();

  sz i = 0;
  for (; i + stride <= (u64) len; i += stride) {
    svuint8_t v = svld1_u8(pg, buf + i);
    if (fold) v = sve2_fold_vec(pg, v);

    svuint8_t pcnt     = svcnt_u8_x(pg, v);
    svuint8_t v_shr1   = svlsr_n_u8_x(pg, v, 1);
    svuint8_t and_adj  = svand_u8_x(pg, v, v_shr1);
    svuint8_t pcnt_adj = svcnt_u8_x(pg, and_adj);

    /*  svext shifts the chunk left by one byte (zero-filling) so lane k
        of v_next holds buf[i+k+1]; the lane-0 LSB then pairs with the
        previous chunk's last byte, handled scalarly below.  */
    svuint8_t v_next     = svext_u8(v, svdup_n_u8(0), 1);
    svuint8_t cross      = svand_u8_x(pg,
                             svand_u8_x(pg, v, svdup_n_u8(1u)),
                             svlsr_n_u8_x(pg, v_next, 7));
    u64 ones     = (u64) svaddv_u8(pg, pcnt);
    u64 adj_in   = (u64) svaddv_u8(pg, pcnt_adj);
    u64 cross_in = (u64) svaddv_u8(pg, cross);

    if (st->have_carry) {
      u8 head = fold ? sve2_fold_byte(buf[i]) : buf[i];
      const unsigned prev_lsb = (unsigned)(st->carry_byte & 1u);
      const unsigned curr_msb = (unsigned)((head >> 7) & 1u);
      st->cross_product += (i64)(prev_lsb & curr_msb);
    } else {
      u8 head = fold ? sve2_fold_byte(buf[i]) : buf[i];
      st->first_byte = (u8)((head >> 7) & 1u);
      st->have_first = 1;
    }

    st->bit_hist[1]   += ones;
    st->bit_hist[0]   += stride * 8u - ones;
    st->cross_product += (i64) adj_in + (i64) cross_in;
    st->total_bytes   += stride * 8u;

    for (u64 k = 0; k < stride; k++) {
      u8 mb = fold ? sve2_fold_byte(buf[i + k]) : buf[i + k];
      st->mc_buf[st->mc_pos++] = mb;
      if (st->mc_pos == 6) SVE2_MC_COMMIT(st);
    }

    u8 last_b = buf[i + stride - 1];
    if (fold) last_b = sve2_fold_byte(last_b);
    st->carry_byte = (u8)(last_b & 1u);
    st->last_byte  = (u8)(last_b & 1u);
    st->have_carry = 1;
  }

  for (; i < (u64) len; i++) {
    u8 b = fold ? sve2_fold_byte(buf[i]) : buf[i];
    sve2_consume_bit_byte(st, b);
  }
}

void analyze_bits_sve2(fastent_chunk_state * st, const u8 * buf, sz len) {
  analyze_bits_sve2_impl(st, buf, len, 0);
}

void analyze_bits_fold_sve2(fastent_chunk_state * st, const u8 * buf, sz len) {
  analyze_bits_sve2_impl(st, buf, len, 1);
}

void fold_sve2(u8 * buf, sz len) {
  const u64 stride = svcntb();
  svbool_t pg = svptrue_b8();

  sz i = 0;
  for (; i + stride <= (u64) len; i += stride) {
    svuint8_t v = svld1_u8(pg, buf + i);
    v = sve2_fold_vec(pg, v);
    svst1_u8(pg, buf + i, v);
  }
  for (; i < (u64) len; i++) buf[i] = sve2_fold_byte(buf[i]);
}
