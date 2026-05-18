/*  fastent: the -eee LZ77F estimator.

    A count-only LZ77F match finder (acceleration 1): never emits a
    byte, no output buffer, no match copies.  Produces the exact
    count-only encoded size and three raw tables (per-offset,
    per-match-length, LZ77-unmatched byte histogram).  Compressibility,
    literal order-0 skew, match coverage and offset / length
    concentration are derived at finalize.

    The parse is keyed to a fixed 4 MiB grid on the ABSOLUTE file
    offset: a fresh hash table per block makes B and every table
    exactly additive, so the integer sum-merge is order-independent
    and bit-identical for any thread count, driver, or host.  Drift
    versus a whole-file table-carrying serial parse is <= ~0.055%
    (verdict-neutral): a fresh table only perturbs the first few
    matches at each block start.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "lzest.h"
#include "analyze.h"
#include "fastent-math.h"
#include "chisq.h"

#include <stdlib.h>
#include <string.h>

static inline u32 lz_rd32(const u8 * p) { u32 v;  memcpy(&v, p, 4);  return v; }
static inline u64 lz_rd64(const u8 * p) { u64 v;  memcpy(&v, p, 8);  return v; }
static inline u32 lz_hash4(u32 s) {
  return (s * 2654435761u) >> (32 - FASTENT_LZ_HLOG);
}

/*  Word-at-a-time forward match length.  */
static inline sz lz_count_eq(const u8 * a, const u8 * b, const u8 * lim) {
  const u8 * a0 = a;
  while (a + 8 <= lim) {
    u64 d = lz_rd64(a) ^ lz_rd64(b);
    if (d) return (sz)(a - a0) + (FASTENT_CTZ64(d) >> 3);
    a += 8;  b += 8;
  }
  while (a < lim && *a == *b) { a++;  b++; }
  return (sz)(a - a0);
}

/*  Fold the u16 offset block into the u64 parent and zero it; called
    every FASTENT_LZ_OFFFLUSH matches so a u16 cell cannot wrap.  */
static void lz_off_flush(fastent_lz_acc * a) {
  Fi(65536, a->off_par[i] += a->off_blk[i];  a->off_blk[i] = 0)
  a->since_flush = 0;
}

/*  Parse one grid block [src,src+n) with a fresh table.  n <=
    FASTENT_LZ_GRID; src has >=32 slack bytes for count_eq's word
    over-read, the partial tail takes the scalar loop (no over-read). */
static void lz_parse_block(fastent_lz_acc * a, const u8 * src, sz n) {
  u32 * FASTENT_RESTRICT tbl = a->tbl;
  memset(tbl, 0, (sz) FASTENT_LZ_HSZ * sizeof(u32));

  if (n < (sz)(FASTENT_LZ_MFLIMIT + 1)) {
    a->outsz += n + 1 + (n + 254) / 255;
    a->nlit_run += 1;  a->lit_bytes += n;
    Fi((int) n, a->lit_byte[src[i]]++)
    return;
  }

  const u8 * ip = src, * anchor = src;
  const u8 * const iend = src + n;
  const u8 * const mflimit = iend - FASTENT_LZ_MFLIMIT;
  const u8 * const matchlimit = iend - FASTENT_LZ_LASTLIT;

  tbl[lz_hash4(lz_rd32(ip))] = (u32)(ip - src);
  ip++;
  u32 forwardH = lz_hash4(lz_rd32(ip));

  for (;;) {
    const u8 * match;
    const u8 * forwardIp = ip;
    int step = 1, searchMatchNb = 1 << FASTENT_LZ_SKIP;
    for (;;) {
      u32 h = forwardH, cur = (u32)(forwardIp - src), mi = tbl[h];
      ip = forwardIp;
      forwardIp += step;
      step = (searchMatchNb++ >> FASTENT_LZ_SKIP);
      if (forwardIp > mflimit) goto last_literals;
      forwardH = lz_hash4(lz_rd32(forwardIp));
      tbl[h] = cur;
      /*  Prefetch the next probe's match bytes so its random miss
          overlaps this iter's rd32(match).  Pure hint, bit-exact.  */
      FASTENT_PREFETCH(src + tbl[forwardH]);
      match = src + mi;
      if (mi + FASTENT_LZ_DISTMAX < cur) continue;     /*  out of win  */
      if (lz_rd32(match) == lz_rd32(ip)) break;        /*  4-byte key  */
    }

    {                                            /*  literal run        */
      sz litlen = (sz)(ip - anchor);
      a->outsz += 1;
      if (litlen >= 15) a->outsz += 1 + (litlen - 15) / 255;
      a->outsz += litlen;
      a->lit_bytes += litlen;  a->nlit_run++;
      for (const u8 * p = anchor; p < ip; p++) a->lit_byte[*p]++;
    }
    {                                            /*  match              */
      u32 offset = (u32)(ip - match);
      sz mlen = lz_count_eq(ip + FASTENT_LZ_MINMATCH,
                            match + FASTENT_LZ_MINMATCH, matchlimit);
      sz total = mlen + FASTENT_LZ_MINMATCH;
      a->outsz += 2;
      if (mlen >= 15) a->outsz += 1 + (mlen - 15) / 255;
      a->match_bytes += total;  a->nmatch++;
      a->off_blk[offset]++;
      if (++a->since_flush == FASTENT_LZ_OFFFLUSH) lz_off_flush(a);
      a->mlen_full[total < 256 ? total : 255]++;
      ip += total;
    }
    anchor = ip;
    if (ip >= mflimit) goto last_literals;
    tbl[lz_hash4(lz_rd32(ip - 2))] = (u32)(ip - 2 - src);
    forwardH = lz_hash4(lz_rd32(ip));
  }

last_literals:
  {
    sz litlen = (sz)(iend - anchor);
    a->outsz += 1;
    if (litlen >= 15) a->outsz += 1 + (litlen - 15) / 255;
    a->outsz += litlen;
    a->lit_bytes += litlen;  a->nlit_run++;
    for (const u8 * p = anchor; p < iend; p++) a->lit_byte[*p]++;
  }
  lz_off_flush(a);
}

/*  Lazily grow the block buffer (grid + 32 slack for count_eq) and
    the hash table.  Returns 0 on success, -1 on OOM.  */
static int lz_ensure(fastent_lz_acc * a) {
  if (a->oom) return -1;
  if (!a->tbl) {
    a->tbl = (u32 *) malloc((sz) FASTENT_LZ_HSZ * sizeof(u32));
    if (!a->tbl) { a->oom = 1;  return -1; }
  }
  if (!a->blk) {
    a->blk = (u8 *) malloc((sz) FASTENT_LZ_GRID + 32u);
    if (!a->blk) { a->oom = 1;  return -1; }
  }
  return 0;
}

void fastent_lz_acc_init(fastent_lz_acc * a, u64 abs_base) {
  memset(a, 0, sizeof(*a));
  a->abs_base = abs_base;
  a->abs_pos  = abs_base;
  a->blk_off  = abs_base;
}

/*  Buffer len bytes (from absolute a->abs_pos) into the current grid
    block, parsing and resetting at each absolute 4 MiB grid line.  */
int fastent_lz_acc_feed(fastent_lz_acc * a, const u8 * buf, sz len) {
  if (len == 0) return 0;
  if (lz_ensure(a) != 0) return -1;
  sz pos = 0;
  while (pos < len) {
    /*  Bytes left until the next absolute grid boundary.  */
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
      lz_parse_block(a, a->blk, a->blk_len);
      a->blk_len = 0;
      a->blk_off = a->abs_pos;
    }
  }
  return 0;
}

/*  Parse the trailing partial block (file tail below a grid line).
    count_eq's over-read is bounded by the grid+32 slack, and a short
    tail (< MFLIMIT+1) takes the scalar-only branch.  */
int fastent_lz_acc_flush(fastent_lz_acc * a) {
  if (a->oom) return -1;
  if (a->blk_len) {
    if (lz_ensure(a) != 0) return -1;
    lz_parse_block(a, a->blk, a->blk_len);
    a->blk_len = 0;
  }
  return 0;
}

void fastent_lz_acc_merge(fastent_lz_acc * dst, const fastent_lz_acc * src) {
  dst->outsz       += src->outsz;
  dst->nmatch      += src->nmatch;
  dst->nlit_run    += src->nlit_run;
  dst->lit_bytes   += src->lit_bytes;
  dst->match_bytes += src->match_bytes;
  if (src->oom) dst->oom = 1;
  Fi(65536, dst->off_par[i] += src->off_par[i])
  Fi(256, dst->mlen_full[i] += src->mlen_full[i];
          dst->lit_byte[i]  += src->lit_byte[i])
}

void fastent_lz_acc_free(fastent_lz_acc * a) {
  free(a->tbl);  a->tbl = NULL;
  free(a->blk);  a->blk = NULL;
}

struct fastent_lz77f_tables * fastent_lz77f_tables_alloc(void) {
  return (struct fastent_lz77f_tables *)
         calloc(1, sizeof(struct fastent_lz77f_tables));
}

void fastent_lz77f_tables_free(struct fastent_lz77f_tables * t) {
  free(t);
}

/*  outsz_rand(n): exact count-only size of a single all-literal
    LZ77F stream (the incompressible-data baseline).  */
static u64 lz_outsz_rand(u64 n) {
  u64 s = 1 + n;
  if (n >= 15) s += 1 + (n - 15) / 255;
  return s;
}

/*  Shannon entropy (bits) of a count table; libm-free via
    fastent_entropy_term, bit-identical across hosts.  */
static f64 lz_entropy_bits(const u64 * cnt, int n) {
  u64 tot = 0;
  Fi(n, tot += cnt[i])
  if (tot == 0) return 0.0;
  const f64 M = (f64) tot;
  f64 h = 0.0;
  Fi(n, if (cnt[i]) h += fastent_entropy_term((f64) cnt[i] / M))
  return h;
}

void fastent_lz_finalize(
    const fastent_lz_acc * a, u64 n, struct fastent_result * out) {
  /*  Snapshot off_par with the pending u16 block folded in (a merged
      acc has since_flush==0; fold defensively anyway).  */
  u64 nmatch    = a->nmatch;
  u64 lit_bytes = a->lit_bytes;
  u64 B         = a->outsz;

  out->lz_cr_excess = out->lz_lit_h = out->lz_lit_kl = 0.0;
  out->lz_match_cov = out->lz_off_conc = out->lz_mlen_excess = 0.0;
  out->lz_lit_chi = out->lz_lit_chi_p = 0.0;
  out->lz_deviation = 0.0;
  out->lz_nmatch = nmatch;
  out->lz_megamatch = 0;

  if (n == 0) return;

  /*  S1: compressibility excess vs the incompressible baseline.  */
  {
    u64 rnd = lz_outsz_rand(n);
    f64 ex  = (rnd > B) ? (f64)(rnd - B) / (f64) n : 0.0;
    out->lz_cr_excess = ex;
  }

  /*  S2: literal byte-value Shannon entropy and KL to uniform
      (8 - H_lit; verified identity for a 256-symbol alphabet).  */
  {
    f64 hl = lz_entropy_bits(a->lit_byte, 256);
    out->lz_lit_h  = hl;
    out->lz_lit_kl = 8.0 - hl;
  }

  /*  S3: match coverage = fraction of bytes inside an LZ77 match.  */
  out->lz_match_cov = 1.0 - (f64) lit_bytes / (f64) n;

  /*  Offset concentration: 1 - H(off_par)/log2(65535) over nonzero
      cells.  nmatch < 2 -> 0.0 (continuous limit; resolves 0/0).  */
  if (nmatch >= 2) {
    f64 ho = lz_entropy_bits(a->off_par, 65536);
    f64 hmax = fastent_log2((f64) 65535);
    f64 conc = (hmax > 0.0) ? 1.0 - ho / hmax : 0.0;
    if (conc < 0.0) conc = 0.0;
    if (conc > 1.0) conc = 1.0;
    out->lz_off_conc = conc;
  }

  /*  Mean match length minus MINMATCH; nmatch < 2 -> 0.0 limit.  */
  if (nmatch >= 2) {
    u64 cnt = 0, sum = 0;
    Fi(256, cnt += a->mlen_full[i];  sum += (u64) i * a->mlen_full[i])
    f64 meanlen = cnt ? (f64) sum / (f64) cnt : 0.0;
    out->lz_mlen_excess = meanlen - (f64) FASTENT_LZ_MINMATCH;
  }

  /*  Literal chi-square vs uniform, df = 255; advisory order-0
      companion (separate from the compressibility headline).  */
  {
    u64 tot = 0;
    Fi(256, tot += a->lit_byte[i])
    if (tot > 0) {
      const f64 ek = (f64) tot / 256.0;
      f64 chi = 0.0;
      Fi(256,
         const f64 d = (f64) a->lit_byte[i] - ek;
         chi += (d * d) / ek)
      out->lz_lit_chi   = chi;
      out->lz_lit_chi_p = fastent_chisq_tail_df(chi, 255);
    } else {
      out->lz_lit_chi   = 0.0;
      out->lz_lit_chi_p = 1.0;
    }
  }

  /*  Mega-match: a periodic/duplicate input collapses to one match
      (nmatch<2), so conc/mlen hit their 0 limits while S1=S3~1.  Flag
      it so output does not read conc=0 as 'no structure'.  */
  if (nmatch < 2 && out->lz_match_cov >= 0.5)
    out->lz_megamatch = 1;

  /*  Headline z.  dev = max(S1, S2/8, S3); sigma0 = max(2/n, 2^-7);
      z = dev / sigma0.  Badge via z_badge_ (z<2 PASS .. z>=3 FAIL).  */
  {
    f64 s1 = out->lz_cr_excess;
    f64 s2 = out->lz_lit_kl / 8.0;
    f64 s3 = out->lz_match_cov;
    f64 dev = s1 > s2 ? s1 : s2;
    if (s3 > dev) dev = s3;
    f64 sg = 2.0 / (f64) n;
    const f64 floor_sg = 1.0 / 128.0;     /*  2^-7  */
    if (sg < floor_sg) sg = floor_sg;
    out->lz_deviation = dev / sg;
  }

  /*  Every step above is a single rounded op (no fused a*b+-c, libm-
      free), so the result is bit-identical across hosts without a
      volatile product barrier.  */

  if (out->lz) {
    memcpy(out->lz->off_par,   a->off_par,   sizeof(out->lz->off_par));
    memcpy(out->lz->mlen_full, a->mlen_full, sizeof(out->lz->mlen_full));
    memcpy(out->lz->lit_byte,  a->lit_byte,  sizeof(out->lz->lit_byte));
  }
}
