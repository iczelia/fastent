/*  fastent: AArch64 SVE batched Berlekamp-Massey (svcntd windows per
    pass).  SVE has a runtime vector length, so the lane count is
    svcntd(); each pass scores that many windows under a whilelt
    predicate, bit-identical to the scalar reference per window.
    SVE vectors are sizeless (no C arrays), so the eight polynomial
    words are eight named registers expanded by an X-macro.

    Copyright (C) 2023-2026 Kamila Szewczyk.  GPLv3-only (see COPYING).  */

#include "common.h"
#include "bm.h"

#if defined(__ARM_BIG_ENDIAN) || \
    (defined(__BYTE_ORDER__) && __BYTE_ORDER__ == __ORDER_BIG_ENDIAN__)
#error "fastent SVE BM variant is not supported on big-endian ARM"
#endif

#include <arm_sve.h>
#include <stdlib.h>
#include <string.h>

/*  Each window occupies one u64 lane; the eight polynomial words are
    eight scalable vectors.  The Massey two-register form carries
    bs_ = x^(N-mm) B advanced by a uniform 1-bit poly *x per step, so
    the discrepancy update is the bare c ^= bs_ with no per-lane
    variable shift, keeping the lanes independent and the per-window L
    bit-identical to the scalar reference.  */

#define WB 8u   /*  FASTENT_BM_W64 polynomial words  */

/*  W8(M) expands M(0)..M(7) for the per-word lane operations.  */
#define W8(M) M(0) M(1) M(2) M(3) M(4) M(5) M(6) M(7)

/*  poly *x on the 8-word MSB-first polynomial named V0..V7: word w
    gets (w>>1) | (w-1 << 63), word 0 just >>1.  */
#define BM_POLX(pg, V)                                                  \
  do {                                                                  \
    V##7 = svorr_u64_x(pg, svlsr_n_u64_x(pg, V##7, 1),                   \
                       svlsl_n_u64_x(pg, V##6, 63));                     \
    V##6 = svorr_u64_x(pg, svlsr_n_u64_x(pg, V##6, 1),                   \
                       svlsl_n_u64_x(pg, V##5, 63));                     \
    V##5 = svorr_u64_x(pg, svlsr_n_u64_x(pg, V##5, 1),                   \
                       svlsl_n_u64_x(pg, V##4, 63));                     \
    V##4 = svorr_u64_x(pg, svlsr_n_u64_x(pg, V##4, 1),                   \
                       svlsl_n_u64_x(pg, V##3, 63));                     \
    V##3 = svorr_u64_x(pg, svlsr_n_u64_x(pg, V##3, 1),                   \
                       svlsl_n_u64_x(pg, V##2, 63));                     \
    V##2 = svorr_u64_x(pg, svlsr_n_u64_x(pg, V##2, 1),                   \
                       svlsl_n_u64_x(pg, V##1, 63));                     \
    V##1 = svorr_u64_x(pg, svlsr_n_u64_x(pg, V##1, 1),                   \
                       svlsl_n_u64_x(pg, V##0, 63));                     \
    V##0 = svlsr_n_u64_x(pg, V##0, 1);                                   \
  } while (0)

/*  Per-lane parity of a u64 vector, broadcast to a full-lane mask
    (all-ones where parity is 1).  Fold 64->1 by xor-halving, then
    expand the low bit.  */
static inline svuint64_t bm_parmask_(svbool_t pg, svuint64_t a) {
  a = sveor_u64_x(pg, a, svlsr_n_u64_x(pg, a, 32));
  a = sveor_u64_x(pg, a, svlsr_n_u64_x(pg, a, 16));
  a = sveor_u64_x(pg, a, svlsr_n_u64_x(pg, a, 8));
  a = sveor_u64_x(pg, a, svlsr_n_u64_x(pg, a, 4));
  a = sveor_u64_x(pg, a, svlsr_n_u64_x(pg, a, 2));
  a = sveor_u64_x(pg, a, svlsr_n_u64_x(pg, a, 1));
  a = svand_n_u64_x(pg, a, 1);
  return svsub_u64_x(pg, svdup_n_u64(0), a);
}

/*  Pack one 64-byte window into 8 u64, MSB-first (byte i high to low,
    bit 7 of byte 0 is poly coefficient 0).  */
static inline void bm_pack_(const u8 * src, u64 s[WB]) {
  Fi((int) WB, s[i] = 0)
  for (u32 i = 0; i < FASTENT_BM_WB; i++)
    s[i >> 3] |= (u64) src[i] << (56u - 8u * (i & 7u));
}

/*  Process `lanes` (<= svcntd) full M-bit windows; write L into Lout. */
static void bm_batch_(const u8 * src, sz lanes, u32 * Lout) {
  u64 (*sp)[WB] = (u64 (*)[WB]) malloc((sz) lanes * sizeof(*sp));
  u64 * nbuf = (u64 *) malloc((sz) lanes * sizeof(u64));
  i64 * L = (i64 *) malloc((sz) lanes * sizeof(i64));
  if (!sp || !nbuf || !L) { free(sp);  free(nbuf);  free(L);  return; }
  for (sz i = 0; i < lanes; i++) {
    bm_pack_(src + i * FASTENT_BM_WB, sp[i]);
    L[i] = 0;
  }

  const svbool_t pg = svwhilelt_b64((u64) 0, (u64) lanes);
  const svuint64_t Z = svdup_n_u64(0);
  svuint64_t c0=Z,c1=Z,c2=Z,c3=Z,c4=Z,c5=Z,c6=Z,c7=Z;
  svuint64_t s0=Z,s1=Z,s2=Z,s3=Z,s4=Z,s5=Z,s6=Z,s7=Z;
  svuint64_t r0=Z,r1=Z,r2=Z,r3=Z,r4=Z,r5=Z,r6=Z,r7=Z;
  svuint64_t t0,t1,t2,t3,t4,t5,t6,t7;
  svuint64_t one63 = svdup_n_u64((u64) 1 << 63);
  c0 = one63;
  s0 = one63;
  BM_POLX(pg, s);                /*  bs_ at start of N=0 is x^1 * 1  */

  for (u32 N = 0; N < FASTENT_BM_M; N++) {
    BM_POLX(pg, r);
    u32 wi = N >> 6, sh = 63u - (N & 63u);
    for (sz i = 0; i < lanes; i++)
      nbuf[i] = ((sp[i][wi] >> sh) & 1ull) << 63;
    r0 = svorr_u64_x(pg, r0, svld1_u64(pg, nbuf));

    svuint64_t acc = Z;
    #define ACC(w) acc = sveor_u64_x(pg, acc, \
                          svand_u64_x(pg, c##w, r##w));
    W8(ACC)
    #undef ACC
    svuint64_t dmask = bm_parmask_(pg, acc);
    svbool_t dm = svcmpne_n_u64(pg, dmask, 0);

    #define SAVE(w) t##w = c##w;
    W8(SAVE)
    #undef SAVE
    #define UPD(w) c##w = sveor_u64_x(pg, c##w, \
                          svand_u64_x(pg, s##w, dmask));
    W8(UPD)
    #undef UPD

    /*  Length change where d set and 2L <= N.  2L<=N is N-2L>=0;
        with L,N small the i64 lane compare is exact.  */
    svint64_t Lv = svld1_s64(pg, L);
    svint64_t twoL = svadd_s64_x(pg, Lv, Lv);
    /*  2L <= N: a true le predicate, intersected with the
        discrepancy predicate.  */
    svbool_t le = svcmple_n_s64(pg, twoL, (i64) N);
    svbool_t chg = svand_b_z(pg, dm, le);
    #define SEL(w) s##w = svsel_u64(chg, t##w, s##w);
    W8(SEL)
    #undef SEL
    /*  L <- N + 1 - L where chg.  */
    svint64_t newL = svsubr_n_s64_x(pg, Lv, (i64) N + 1);
    Lv = svsel_s64(chg, newL, Lv);
    svst1_s64(pg, L, Lv);

    BM_POLX(pg, s);
  }
  for (sz i = 0; i < lanes; i++) Lout[i] = (u32) L[i];
  free(sp);  free(nbuf);  free(L);
}

/*  Score nfull full windows, svcntd at a time.  Returns the count
    actually scored (the largest multiple of svcntd <= nfull); the
    caller handles the tail with the scalar reference.  */
sz fastent_bm_windows_sve(const u8 * src, sz nfull, u32 * Lout) {
  const sz vl = (sz) svcntd();
  sz g = nfull - (nfull % vl);
  for (sz i = 0; i < g; i += vl)
    bm_batch_(src + i * FASTENT_BM_WB, vl, Lout + i);
  return g;
}
