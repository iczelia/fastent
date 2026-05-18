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
  u32 c = b;
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

static void analyze_sve2_byte_impl(
    fastent_chunk_state * st, const u8 * buf, sz len, int fold) {
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
        st->bank[(u32)(i + k) & (FASTENT_BANKS - 1)][b]++;
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
    st->bank[(u32) i & (FASTENT_BANKS - 1)][b]++;
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
  const u32 ones_in_byte = FASTENT_POPCOUNT32(b);
  st->bit_hist[1] += ones_in_byte;
  st->bit_hist[0] += 8u - ones_in_byte;
  const u32 within = FASTENT_POPCOUNT32(b & (b >> 1));
  st->cross_product += (i64) within;
  if (st->have_carry) {
    const u32 prev_lsb = (u32)(st->carry_byte & 1u);
    const u32 curr_msb = (u32)((b >> 7) & 1u);
    st->cross_product += (i64)(prev_lsb & curr_msb);
  } else {
    st->first_byte = (u8)((b >> 7) & 1u);
    st->have_first = 1;
  }
  st->carry_byte = (u8)(b & 1u);
  st->last_byte  = (u8)(b & 1u);
  st->have_carry = 1;
  st->mc_buf[st->mc_pos++] = b;
  if (st->mc_pos == 6) SVE2_MC_COMMIT(st);
  st->total_bytes += 8;
}

static void analyze_bits_sve2_impl(
    fastent_chunk_state * st, const u8 * buf, sz len, int fold) {
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
      const u32 prev_lsb = (u32)(st->carry_byte & 1u);
      const u32 curr_msb = (u32)((head >> 7) & 1u);
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

/*  SVE2 -ee level-2 digram+run kernel, bit-identical to scalar for any
    -j and VL: never folds, key=(dg_prev<<8)|cur, plane=abs-index parity,
    run via CTZ64 = fastent_lr_one.  Boundary packed to u64 arithmetically
    (endian-independent), scalar inc-mem scatter (Zen4-scatter rationale).  */

#define SVE2_DG_MAXVL 256u   /*  SVE max VL is 2048 bits = 256 bytes  */

void digram_bytes_sve2(
    fastent_chunk_state * st, const u8 * FASTENT_RESTRICT buf, sz len) {
  if (len == 0) return;
  u32 * FASTENT_RESTRICT t = st->dg_u32;

  sz i0;
  if (st->dg_have) {
    i0 = 0;
  } else {
    fastent_lr_one(st, buf[0]);   /*  bootstrap: buf[0], no left pair  */
    st->dg_prev = buf[0];
    st->dg_have = 1;
    i0 = 1;
  }
  if (i0 >= (sz) len) { st->dg_prev = buf[len - 1]; return; }

  /*  Run scan state: the run currently open starts at runstart with
      symbol runsym; closed runs are flushed to fastent_lr_run.  */
  sz  runstart = i0;
  u32 runsym   = buf[i0];

  /*  Scalar prologue: the SVE window needs buf[base-1], so starts at
      index >= 1.  i0 in {0,1}; if i0==0 emit the index-0 pair (left =
      carried dg_prev) so the window can start at 1.  Index 0 has no
      run boundary.  If i0==1 nothing emitted here.  */
  sz k = i0;
  if (k == 0) {
    u32 c = buf[0];
    t[((u32)(0u & 1u)) * FASTENT_BG_TABLE
      + (((u32) st->dg_prev << 8) | c)]++;
    k = 1;
  }

  const u64 W = svcntb();
  svbool_t pg = svptrue_b8();

  /*  Boundary words: W bits, bit j set iff buf[k+j] != buf[k+j-1].
      Materialised from the not-equal predicate as 0/1 bytes then
      packed arithmetically (no memory reinterpret -> endian-safe).  */
  FASTENT_ALIGN(64) u8  neq[SVE2_DG_MAXVL];
  FASTENT_ALIGN(64) u16 keys[SVE2_DG_MAXVL];

  while (k >= 1 && (u64) k + W <= (u64) len) {
    svuint8_t v  = svld1_u8(pg, buf + k);
    svuint8_t vp = svld1_u8(pg, buf + k - 1);

    /*  P1: equality bitmap.  ne predicate -> 0/1 lane bytes.  */
    svbool_t  ne   = svcmpne_u8(pg, v, vp);
    svuint8_t nev  = svdup_n_u8_z(ne, 1u);
    svst1_u8(pg, neq, nev);

    /*  P2: 16-bit digram keys, VL-wide.  zip1/zip2 interleave v,vp;
        reinterpreted u16 is v|(vp<<8) == (left<<8)|cur on little-endian
        (this TU is LE-only, see the #error guard), matching the scalar
        arithmetic key exactly.  */
    {
      svuint16_t lo = svreinterpret_u16_u8(svzip1_u8(v, vp));
      svuint16_t hi = svreinterpret_u16_u8(svzip2_u8(v, vp));
      svbool_t pg16 = svptrue_b16();
      svst1_u16(pg16, keys,                 lo);
      svst1_u16(pg16, keys + svcnth(),      hi);
    }

    /*  Scalar inc-mem scatter from the staged keys with one-ahead
        prefetch and absolute-index parity round-robin.  The launder
        defeats any FP/predicate -> GP extract chain on the staged
        buffer (same intent as the x86 template's FASTENT_LAUNDER).  */
    {
      const u16 * FASTENT_RESTRICT sp = keys;
      __asm__("" : "+r"(sp) :: "memory");
      u32 * FASTENT_RESTRICT t0 = t;
      u32 * FASTENT_RESTRICT t1 = t + FASTENT_BG_TABLE;
      const u32 par = (u32)(k & 1u);
      u64 j;
      for (j = 0; j < W; j++) {
        u32 key = sp[j];
        if (j + 8 < W) FASTENT_PREFETCH(&t0[sp[j + 8]]);
        if (((u32) j ^ par) & 1u) t1[key]++;
        else                      t0[key]++;
      }
    }

    /*  Boundary bit j (abs k..k+W-1): buf[k+j]!=buf[k+j-1].  Dense
        fast path: W singleton runs; the W-2 distinct singletons take
        the different-symbol branch so only the first raises lr_max,
        leaving lr_sym=buf[k+W-2].  Bit-identical, W=svcntb()>=16.  */
    {
      int dense = 1;
      u64 j;
      for (j = 0; j < W; j++) if (!neq[j]) { dense = 0; break; }
      if (dense) {
        fastent_lr_run(st, runsym, (u64)((sz) k - (sz) runstart));
        fastent_lr_run(st, buf[k], 1u);
        st->lr_sym = buf[k + W - 2];      /*  collapse singletons  */
        runstart = (sz)((u64) k + W - 1);
        runsym   = buf[runstart];
      } else {
        /*  Pack neq into W-bit boundary words and iterate via CTZ.  */
        u64 base = 0;
        while (base < W) {
          u64 chunk = W - base;
          if (chunk > 64u) chunk = 64u;
          u64 bw = 0, bit;
          for (bit = 0; bit < chunk; bit++)
            if (neq[base + bit]) bw |= (u64) 1u << bit;
          while (bw) {
            const sz bpos = (sz)((u64) k + base + FASTENT_CTZ64(bw));
            fastent_lr_run(st, runsym,
                           (u64)((sz) bpos - (sz) runstart));
            runstart = bpos;
            runsym   = buf[bpos];
            bw &= bw - 1;
          }
          base += chunk;
        }
      }
    }
    k = (sz)((u64) k + W);
  }

  /*  Scalar tail: remaining pairs + remaining boundaries.  */
  for (; (sz) k < (sz) len; k++) {
    u32 c = buf[k];
    t[(u32)(k & 1u) * FASTENT_BG_TABLE
      + (((u32) buf[k - 1] << 8) | c)]++;
    if (buf[k] != buf[k - 1]) {
      fastent_lr_run(st, runsym, (u64)((sz) k - (sz) runstart));
      runstart = k;
      runsym   = buf[k];
    }
  }

  /*  Close the final open run [runstart .. len-1].  */
  fastent_lr_run(st, runsym, (u64)((sz) len - (sz) runstart));
  st->dg_prev = buf[len - 1];
}
