/*  fastent: the -eee windowed linear-complexity estimator.

    A count-only GF(2) Berlekamp-Massey scorer: each 512-bit window's
    linear complexity L is scored on the shared 4 MiB absolute grid
    (64 divides the grid so windows never straddle: exact integer
    sum-merge, zero drift).  Catches LFSR-class and low-bit linear
    recurrences, not truncated-high-byte LCGs.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "bm.h"
#include "analyze.h"
#include "fastent-math.h"
#include "chisq.h"
#if defined(HAVE_SSE41) || defined(HAVE_AVX2) || defined(HAVE_AVX512) \
 || defined(HAVE_NEON) || defined(HAVE_SVE2)
#define FASTENT_BM_HAVE_VEC 1
#include "port-cpu.h"
#endif

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*  IEEE sqrt is correctly rounded, so bit-identical across hosts.  */
static inline f64 fastent_bm_sqrt(f64 x) { return sqrt(x); }

#if defined(__GNUC__) && !defined(__TINYC__)
  #define FASTENT_PARITY64(x) ((u32) __builtin_parityll((u64)(x)))
  #define FASTENT_FALLTHROUGH __attribute__((fallthrough))
#else
  #define FASTENT_PARITY64(x) (FASTENT_POPCOUNT64((u64)(x)) & 1u)
  #define FASTENT_FALLTHROUGH ((void) 0)
#endif

/*  rev word w at step N: bit 63-k = source bit N-64w-k, MSB-first.  */
static inline u64 fastent_bm_revword(const u64 * s, u32 N, u32 w) {
  i64 base = (i64) N - (i64) (w << 6);
  u64 r = 0;
  for (u32 k = 0; k < 64u && base - (i64) k >= 0; k++) {
    i64 idx = base - (i64) k;
    r |= ((s[idx >> 6] >> (63u - ((u32) idx & 63u))) & 1ull) << (63u - k);
  }
  return r;
}

/*  L of one m-bit window via GF(2) Berlekamp-Massey.  c and the bits
    share the MSB-first layout; rev is the window read backwards from
    N, so the discrepancy is parity(c & rev).  Returns L in [0, m].  */
static u32 fastent_bm_window(const u64 * s, u32 m) {
  u64 c[FASTENT_BM_W64], b[FASTENT_BM_W64], t[FASTENT_BM_W64];
  u64 rev[FASTENT_BM_W64];
  const u32 W = FASTENT_BM_W64;
  Fi((int) W, c[i] = b[i] = rev[i] = 0)
  c[0] = b[0] = (u64) 1 << 63;
  u32 L = 0, act = 1u, bw = 0;
  i64 mm = -1;

  for (u32 N = 0; N < m; N++) {
    /*  C has degree <= L, so only nw = L/64 + 1 words are live; rev's
        live prefix shifts in place, a newly live word is built from s. */
    u32 nw = (L >> 6) + 1u, old = act;
    u64 acc = 0;
    /*  nw is tiny (1..4); the fall-through switch drops the counter.  */
    #define RW(w) do { rev[w] = (rev[w] >> 1) | (rev[(w) - 1] << 63); \
                       acc ^= c[w] & rev[w]; } while (0)
    switch (old) {
      case 8: RW(7);  FASTENT_FALLTHROUGH;
      case 7: RW(6);  FASTENT_FALLTHROUGH;
      case 6: RW(5);  FASTENT_FALLTHROUGH;
      case 5: RW(4);  FASTENT_FALLTHROUGH;
      case 4: RW(3);  FASTENT_FALLTHROUGH;
      case 3: RW(2);  FASTENT_FALLTHROUGH;
      case 2: RW(1);  FASTENT_FALLTHROUGH;
      default: break;
    }
    #undef RW
    rev[0] >>= 1;
    rev[0] |= ((s[N >> 6] >> (63u - (N & 63u))) & 1ull) << 63;
    acc ^= c[0] & rev[0];
    while (act < nw) {
      rev[act] = fastent_bm_revword(s, N, act);
      acc ^= c[act] & rev[act];
      act++;
    }
    u32 d = (u32) FASTENT_PARITY64(acc);

    if (d) {
      memcpy(t, c, sizeof t);
      u32 shift = (u32)((i64) N - mm);
      u32 ws = shift >> 6, bs = shift & 63u;
      /*  Clamp the XOR span to the live words of C and shifted B.  */
      u32 cw = L >> 6, bwe = bw + ws + 1u;
      u32 we = (cw > bwe ? cw : bwe) + 1u;
      if (we > W) we = W;
      if (bs == 0) {
        for (u32 w = we; w-- > ws; ) c[w] ^= b[w - ws];
      } else {
        for (u32 w = we; w-- > ws; ) {
          u64 v = b[w - ws] >> bs;
          if (w - ws > 0) v |= b[w - ws - 1] << (64u - bs);
          c[w] ^= v;
        }
      }
      if (2u * L <= N) {
        bw = L >> 6;
        L = N + 1u - L;
        mm = (i64) N;
        memcpy(b, t, sizeof t);
      }
    }
  }
  return L;
}

static inline u64 fastent_bm_load_be64_(const u8 * p) {
  u64 v;
  memcpy(&v, p, 8);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return v;
#else
  return FASTENT_BSWAP64(v);
#endif
}

static void fastent_bm_pack(const u8 * src, u32 mb, u64 * s) {
  if (mb == FASTENT_BM_WB) {
    Fi(FASTENT_BM_W64, s[i] = fastent_bm_load_be64_(src + (sz) i * 8))
    return;
  }
  Fi(FASTENT_BM_W64, s[i] = 0)
  for (u32 i = 0; i < mb; i++) {
    u32 wi = i >> 3;
    u32 sh = 56u - 8u * (i & 7u);
    s[wi] |= (u64) src[i] << sh;
  }
}

/*  Fold one full window's L into the integer accumulators.  */
static inline void fastent_bm_account(fastent_bm_acc * a, u32 L) {
  a->windows++;
  a->meanl_sum += L;
  u32 bin = (u32)(((u64) L * FASTENT_BM_LBINS) / (FASTENT_BM_M + 1u));
  if (bin >= FASTENT_BM_LBINS) bin = FASTENT_BM_LBINS - 1u;
  a->lhist[bin]++;
  /*  NIST T = (L - mu) + 2/9 (M even); the 7 classes pooled to 6
      (df = 5, odd) by merging both extreme tails into bin 0.  */
  f64 T = ((f64) L - 256.27777777777777) + 0.2222222222222222;
  u32 cls;
  if      (T <= -2.5) cls = 0;
  else if (T <= -1.5) cls = 1;
  else if (T <= -0.5) cls = 2;
  else if (T <=  0.5) cls = 3;
  else if (T <=  1.5) cls = 4;
  else if (T <=  2.5) cls = 5;
  else                cls = 0;
  a->tbin[cls]++;
}

typedef sz (*fastent_bm_scorer)(const u8 *, sz, u32 *);

/*  Pick the widest batched scorer this host can run, widest first,
    scalar (NULL) last.  Mirrors the analyse/fips dispatch order; the
    per-window L is bit-identical by construction so any path is the
    scalar reference's answer.  Picked once per parse_block.  */
static fastent_bm_scorer fastent_bm_pick(void) {
#ifdef FASTENT_BM_HAVE_VEC
  const fastent_cpu_features * c = fastent_cpu_get();
#ifdef HAVE_AVX512
  if (c->avx512f && c->avx512bw) return fastent_bm_windows_avx512;
#endif
#ifdef HAVE_AVX2
  if (c->avx2) return fastent_bm_windows_avx2;
#endif
#ifdef HAVE_SSE41
  if (c->sse42) return fastent_bm_windows_sse;
#endif
#ifdef HAVE_SVE2
  if (c->sve2) return fastent_bm_windows_sve;
#endif
#ifdef HAVE_NEON
  if (c->neon) return fastent_bm_windows_neon;
#endif
#endif
  return NULL;
}

/*  n/64 whole windows; the remainder is the stream's tail window.
    The batched scorer takes a lane group at a time (same per-window
    L); its sub-lane-width tail and non-vector hosts use the scalar
    reference.  */
static void fastent_bm_parse_block(fastent_bm_acc * a, const u8 * src, sz n) {
  sz nw = n / FASTENT_BM_WB;
  sz i = 0;
  fastent_bm_scorer score = fastent_bm_pick();
  if (score && nw >= 2) {
    u32 Lb[FASTENT_BM_BATCH];
    while (nw - i >= 2) {
      sz want = nw - i;
      if (want > FASTENT_BM_BATCH) want = FASTENT_BM_BATCH;
      sz did = score(src + i * FASTENT_BM_WB, want, Lb);
      if (did == 0) break;
      for (sz k = 0; k < did; k++) fastent_bm_account(a, Lb[k]);
      i += did;
    }
  }
  for (; i < nw; i++) {
    u64 s[FASTENT_BM_W64];
    fastent_bm_pack(src + i * FASTENT_BM_WB, FASTENT_BM_WB, s);
    fastent_bm_account(a, fastent_bm_window(s, FASTENT_BM_M));
  }
  sz rem = n - nw * FASTENT_BM_WB;
  if (rem) {
    u64 abs = a->blk_off + (u64) nw * FASTENT_BM_WB;
    if (!a->have_tail || abs >= a->tail_abs) {
      u64 s[FASTENT_BM_W64];
      fastent_bm_pack(src + nw * FASTENT_BM_WB, (u32) rem, s);
      a->tail_abs  = abs;
      a->tail_bits = (u32) rem * 8u;
      a->tail_l    = fastent_bm_window(s, (u32) rem * 8u);
      a->have_tail = 1;
    }
  }
}

static int fastent_bm_ensure(fastent_bm_acc * a) {
  if (a->oom) return -1;
  if (!a->blk) {
    a->blk = (u8 *) malloc((sz) FASTENT_LZ_GRID);
    if (!a->blk) { a->oom = 1;  return -1; }
  }
  return 0;
}

void fastent_bm_acc_init(fastent_bm_acc * a, u64 abs_base) {
  memset(a, 0, sizeof(*a));
  a->abs_base = abs_base;
  a->abs_pos  = abs_base;
  a->blk_off  = abs_base;
}

/*  Re-init for the next grid block, keeping the lazily-grown buffer. */
void fastent_bm_acc_reset(fastent_bm_acc * a, u64 abs_base) {
  u8 * blk = a->blk;
  fastent_bm_acc_init(a, abs_base);
  a->blk = blk;
}

int fastent_bm_acc_feed(fastent_bm_acc * a, const u8 * buf, sz len) {
  if (len == 0) return 0;
  if (fastent_bm_ensure(a) != 0) return -1;
  sz pos = 0;
  while (pos < len) {
    u64 abs = a->abs_pos;
    u64 next_grid = (abs / FASTENT_LZ_GRID + 1) * (u64) FASTENT_LZ_GRID;
    sz  room = (sz)(next_grid - abs);
    sz  take = len - pos;
    if (take > room) take = room;
    memcpy(a->blk + a->blk_len, buf + pos, take);
    a->blk_len += take;
    a->abs_pos += take;
    pos        += take;
    if (a->abs_pos % FASTENT_LZ_GRID == 0) {
      fastent_bm_parse_block(a, a->blk, a->blk_len);
      a->blk_len = 0;
      a->blk_off = a->abs_pos;
    }
  }
  return 0;
}

int fastent_bm_acc_flush(fastent_bm_acc * a) {
  if (a->oom) return -1;
  if (a->blk_len) {
    if (fastent_bm_ensure(a) != 0) return -1;
    fastent_bm_parse_block(a, a->blk, a->blk_len);
    a->blk_len = 0;
  }
  return 0;
}

void fastent_bm_acc_merge(fastent_bm_acc * dst, const fastent_bm_acc * src) {
  dst->windows   += src->windows;
  dst->meanl_sum += src->meanl_sum;
  if (src->oom) dst->oom = 1;
  Fi(FASTENT_BM_LBINS, dst->lhist[i] += src->lhist[i])
  Fi(6, dst->tbin[i] += src->tbin[i])
  /*  Keep the highest-abs tail window so the merge is order-free.  */
  if (src->have_tail &&
      (!dst->have_tail || src->tail_abs >= dst->tail_abs)) {
    dst->tail_abs  = src->tail_abs;
    dst->tail_bits = src->tail_bits;
    dst->tail_l    = src->tail_l;
    dst->have_tail = 1;
  }
}

void fastent_bm_acc_free(fastent_bm_acc * a) {
  free(a->blk);  a->blk = NULL;
}

/*  NIST SP800-22 random-sequence L expectation; libm-free.  */
static f64 fastent_bm_mu(u32 m) {
  f64 alt = (m & 1u) ? 1.0 : -1.0;
  f64 base = (f64) m / 2.0 + (9.0 + alt) / 36.0;
  f64 corr = ((f64) m / 3.0 + 2.0 / 9.0);
  f64 scale = 1.0;
  for (u32 k = 0; k < m && scale > 0.0; k++) scale *= 0.5;
  return base - corr * scale;
}

void fastent_bm_finalize(
    const fastent_bm_acc * a, u64 n, struct fastent_result * out) {
  (void) n;
  out->bm_deviation = 0.0;
  out->bm_mean_lc   = 0.0;
  out->bm_mu        = fastent_bm_mu(FASTENT_BM_M);
  out->bm_chi       = 0.0;
  out->bm_chi_p     = 1.0;
  out->bm_windows   = a->windows;
  out->bm_degenerate = 0;
  Fi(FASTENT_BM_LBINS, out->bm_lhist[i] = (u32)(a->lhist[i] > 0xffffffffu
                                                ? 0xffffffffu : a->lhist[i]))

  u64 W = a->windows;
  if (W == 0) {
    /*  No full window: score the tail vs mu(m') if m' >= 64, else
        z = 0 (the total-function value, not the NaN sentinel).  */
    if (a->have_tail && a->tail_bits >= 64u) {
      f64 mp = fastent_bm_mu(a->tail_bits);
      out->bm_mu      = mp;
      out->bm_mean_lc = (f64) a->tail_l;
      const f64 vp = 86.0 / 81.0;
      volatile f64 num = (f64) a->tail_l - mp;
      f64 an = num < 0.0 ? -num : num;
      out->bm_deviation = an / fastent_bm_sqrt(vp);
    }
    return;
  }

  /*  z = |meanL - mu| / sqrt(VarL/W); fused via the volatile barrier.  */
  f64 meanL = (f64) a->meanl_sum / (f64) W;
  out->bm_mean_lc = meanL;
  const f64 mu  = out->bm_mu;
  const f64 var = 86.0 / 81.0;
  volatile f64 nv = meanL - mu;
  f64 num = nv < 0.0 ? -nv : nv;
  f64 sigma = fastent_bm_sqrt(var / (f64) W);
  out->bm_deviation = sigma > 0.0 ? num / sigma : 0.0;

  /*  meanL < 2: near-constant stream; the z still fails it.  */
  if (meanL < 2.0) out->bm_degenerate = 1;

  /*  Advisory NIST class chi-square, df = 5; bin 0 pools the tails.  */
  static const f64 pi6[6] = {
    0.03125, 0.03125, 0.125, 0.5, 0.25, 0.0625
  };
  f64 chi = 0.0;
  Fi(6,
     volatile f64 e = (f64) W * pi6[i];
     volatile f64 v = (f64) a->tbin[i];
     volatile f64 d = v - e;
     volatile f64 term = (d * d) / e;
     chi += term)
  out->bm_chi   = chi;
  out->bm_chi_p = fastent_chisq_tail_df(chi, 5);
}
