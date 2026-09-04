/*  Copyright (C) 2023-2026 Kamila Szewczyk

    This program is free software; you can redistribute it and/or modify
    it under the terms of the GNU General Public License as published by
    the Free Software Foundation, version 3.

    This program is distributed in the hope that it will be useful,
    but WITHOUT ANY WARRANTY; without even the implied warranty of
    MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
    GNU General Public License for more details.

    You should have received a copy of the GNU General Public License
    along with this program. If not, see <http://www.gnu.org/licenses/>.  */

#include "mrank.h"
#include "analyze.h"
#include "chisq.h"

#include <math.h>
#include <stdlib.h>
#include <string.h>

/*  IEEE sqrt is correctly rounded, so bit-identical across hosts.  */
static inline f64 fastent_mrank_sqrt(f64 x) { return sqrt(x); }

/*  NIST sec 2.5 closed form for M = Q = 32: P_32 = prod_{k=1..32} (1 - 2^-k)
    P_31 = 2^-1 * prod_{i=0..30} (1 - 2^(i-32))^2 / (1 - 2^(i-31)) P_lo = 1 -
    P_32 - P_31 Constants are baked at full f64 precision so finalize stays
    cross-host bit-identical (no recomputation drift).  */
#define FASTENT_MRANK_P32 0.28878809515384113
#define FASTENT_MRANK_P31 0.57757619017320461
#define FASTENT_MRANK_PLO 0.13363571467295432

static inline u32 mrank_load_be32_(const u8 * p) {
  u32 v;
  memcpy(&v, p, 4);
#if defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__
  return v;
#elif defined(__GNUC__) && !defined(__TINYC__)
  return (u32) __builtin_bswap32(v);
#else
  return ((v & 0xFF000000u) >> 24) | ((v & 0x00FF0000u) >>  8)
       | ((v & 0x0000FF00u) <<  8) | ((v & 0x000000FFu) << 24);
#endif
}

/*  Rank of one 32x32 GF(2) matrix, rows packed MSB-first into u32.  */
static u32 mrank_rank32_(u32 * rows) {
  u32 r = 0;
  i32 c, i;
  for (c = 31; c >= 0; c--) {
    u32 mask = (u32) 1 << (u32) c;
    u32 piv = 32u;
    for (i = (i32) r; i < 32; i++) {
      if (rows[i] & mask) { piv = (u32) i;  break; }
    }
    if (piv == 32u) continue;
    if (piv != r) { u32 t = rows[r];  rows[r] = rows[piv];  rows[piv] = t; }
    {
      u32 pr = rows[r];
      for (i = (i32) (r + 1u); i < 32; i++) {
        u32 cond = (rows[i] >> (u32) c) & 1u;
        rows[i] ^= ((u32) 0 - cond) & pr;
      }
    }
    r++;
  }
  return r;
}

/*  Fold one full matrix's rank into the integer bins.  */
static inline void mrank_account_(fastent_mrank_acc * a, u32 rank) {
  a->matrices++;
  if      (rank == 32u) a->bins[0]++;
  else if (rank == 31u) a->bins[1]++;
  else                  a->bins[2]++;
}

/*  n/128 whole matrices; any byte tail is dropped (NIST 2.5 discards
    the trailing partial).  */
static void mrank_parse_block_(
    fastent_mrank_acc * a, const u8 * src, sz n) {
  sz nm = n / FASTENT_MRANK_MB;
  i32 i, j;
  Fi((i32) nm,
    u32 rows[32];
    const u8 * p = src + (sz) i * FASTENT_MRANK_MB;
    Fj(32, rows[j] = mrank_load_be32_(p + (sz) j * 4u));
    mrank_account_(a, mrank_rank32_(rows)));
}

static int mrank_ensure_(fastent_mrank_acc * a) {
  if (a->oom) return -1;
  if (!a->blk) {
    a->blk = (u8 *) malloc((sz) FASTENT_LZ_GRID);
    if (!a->blk) { a->oom = 1;  return -1; }
  }
  return 0;
}

void fastent_mrank_acc_init(fastent_mrank_acc * a, u64 abs_base) {
  memset(a, 0, sizeof (*a));
  a->abs_base = abs_base;
  a->abs_pos  = abs_base;
  a->blk_off  = abs_base;
}

/*  Re-init for the next grid block, keeping the lazily-grown buffer.  */
void fastent_mrank_acc_reset(fastent_mrank_acc * a, u64 abs_base) {
  u8 * blk = a->blk;
  fastent_mrank_acc_init(a, abs_base);
  a->blk = blk;
}

int fastent_mrank_acc_feed(fastent_mrank_acc * a, const u8 * buf, sz len) {
  if (len == 0) return 0;
  if (mrank_ensure_(a) != 0) return -1;
  sz pos = 0;
  while (pos < len) {
    u64 abs = a->abs_pos;
    u64 next_grid = (abs / FASTENT_LZ_GRID + 1) * (u64) FASTENT_LZ_GRID;
    sz  room = (sz) (next_grid - abs);
    sz  take = len - pos;
    if (take > room) take = room;
    memcpy(a->blk + a->blk_len, buf + pos, take);
    a->blk_len += take;
    a->abs_pos += take;
    pos        += take;
    if (a->abs_pos % FASTENT_LZ_GRID == 0) {
      mrank_parse_block_(a, a->blk, a->blk_len);
      a->blk_len = 0;
      a->blk_off = a->abs_pos;
    }
  }
  return 0;
}

int fastent_mrank_acc_flush(fastent_mrank_acc * a) {
  if (a->oom) return -1;
  if (a->blk_len) {
    if (mrank_ensure_(a) != 0) return -1;
    mrank_parse_block_(a, a->blk, a->blk_len);
    a->blk_len = 0;
  }
  return 0;
}

void fastent_mrank_acc_merge(
    fastent_mrank_acc * dst, const fastent_mrank_acc * src) {
  i32 i;
  dst->matrices += src->matrices;
  if (src->oom) dst->oom = 1;
  Fi(FASTENT_MRANK_BINS, dst->bins[i] += src->bins[i]);
}

void fastent_mrank_acc_free(fastent_mrank_acc * a) {
  free(a->blk);  a->blk = NULL;
}

void fastent_mrank_compute(
    const fastent_mrank_acc * a, fastent_mrank_summary * s) {
  i32 i;
  /*  Zero the whole struct including padding so a byte-equal memcmp
      between two compute runs is the determinism check.  */
  memset(s, 0, sizeof (*s));
  s->matrices = a->matrices;
  Fi(FASTENT_MRANK_BINS, s->hist[i] = (u32) (a->bins[i] > 0xffffffffu
                                           ? 0xffffffffu : a->bins[i]));
  s->p_r32 = FASTENT_MRANK_P32;
  s->p_r31 = FASTENT_MRANK_P31;
  s->p_rlo = FASTENT_MRANK_PLO;
  s->chi2  = 0.0;
  s->mrank_dev = 0.0;
  s->mrank_underpowered = 0;

  if (a->matrices < (u64) FASTENT_MRANK_MIN) {
    s->mrank_underpowered = 1;
    s->mrank_dev = (f64) NAN;
    return;
  }

  /*  Fused (obs - exp)^2 / exp in fixed bin order, every step through
      a volatile barrier to keep the sum cross-host bit-identical.  */
  f64 N = (f64) a->matrices;
  const f64 P[FASTENT_MRANK_BINS] = {
    FASTENT_MRANK_P32, FASTENT_MRANK_P31, FASTENT_MRANK_PLO
  };
  volatile f64 chi = 0.0;
  Fi(FASTENT_MRANK_BINS,
    volatile f64 e = N * P[i];
    volatile f64 v = (f64) a->bins[i];
    volatile f64 d = v - e;
    volatile f64 t = (d * d) / e;
    chi = chi + t);
  s->chi2 = chi;
  s->mrank_dev = fastent_mrank_sqrt(chi);
}

/*  Write every output field unconditionally: sentinels first (NaN for f64, 0
    for ints), then overwrite with real values, so a NaN sentinel never
    escapes when underpowered.  */
void fastent_mrank_finalize(
    const fastent_mrank_acc * a, u64 n, struct fastent_result * out) {
  i32 i;
  (void) n;
  out->mrank_dev = (f64) NAN;
  out->mrank_chi = (f64) NAN;
  out->mrank_chi_p = (f64) NAN;
  out->mrank_matrices = a->matrices;
  out->mrank_r32 = (u32) (a->bins[0] > 0xffffffffu ? 0xffffffffu : a->bins[0]);
  out->mrank_r31 = (u32) (a->bins[1] > 0xffffffffu ? 0xffffffffu : a->bins[1]);
  out->mrank_rlo = (u32) (a->bins[2] > 0xffffffffu ? 0xffffffffu : a->bins[2]);
  out->mrank_underpowered = 0;

  if (a->matrices < (u64) FASTENT_MRANK_MIN) {
    out->mrank_underpowered = 1;
    return;
  }

  /*  Fused (obs - exp)^2 / exp in fixed bin order; every step through a
      volatile barrier so the f64 sum is cross-host bit-identical.  */
  f64 N = (f64) a->matrices;
  const f64 P[FASTENT_MRANK_BINS] = {
    FASTENT_MRANK_P32, FASTENT_MRANK_P31, FASTENT_MRANK_PLO
  };
  volatile f64 chi = 0.0;
  Fi(FASTENT_MRANK_BINS,
    volatile f64 e = N * P[i];
    volatile f64 v = (f64) a->bins[i];
    volatile f64 d = v - e;
    volatile f64 t = (d * d) / e;
    chi = chi + t);
  out->mrank_chi = chi;
  out->mrank_dev = fastent_mrank_sqrt(chi);
  /*  df = 2 is even; fastent_chisq_tail_df only accepts odd df, so the
      p-value stays the NaN sentinel.  */
}
