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

#include <errno.h>
#include <inttypes.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define CHUNK (1u << 20)  /*  1 MiB write granule.  */

static uint64_t xrng = 0x9E3779B97F4A7C15ULL;

static uint64_t xs64(void) {
  uint64_t x = xrng;
  x ^= x << 13; x ^= x >> 7; x ^= x << 17;
  return xrng = x;
}

static void seed(const char * kind) {
  uint64_t s = 0xCBF29CE484222325ULL;
  const char * p;
  for (p = kind; *p; p++) {
    s ^= (uint8_t) *p;
    s *= 0x100000001B3ULL;
  }
  if (s == 0) s = 0x9E3779B97F4A7C15ULL;
  xrng = s;
}

typedef void (*gen_fn)(uint8_t *, size_t, uint64_t);

static void gen_random(uint8_t * b, size_t n, uint64_t pos) {
  (void) pos;
  size_t i = 0;
  while (i + 8 <= n) {
    uint64_t r = xs64();
    memcpy(b + i, &r, 8);
    i += 8;
  }
  while (i < n) b[i++] = (uint8_t) xs64();
}

static void gen_zeros(uint8_t * b, size_t n, uint64_t pos) {
  (void) pos; memset(b, 0x00, n);
}

static void gen_ones(uint8_t * b, size_t n, uint64_t pos) {
  (void) pos; memset(b, 0xFF, n);
}

static void gen_counter(uint8_t * b, size_t n, uint64_t pos) {
  size_t i;
  for (i = 0; i < n; i++) b[i] = (uint8_t) ((pos + i) & 0xFFu);
}

static const char dna_alpha[4] = { 'A', 'C', 'G', 'T' };

static void gen_dna(uint8_t * b, size_t n, uint64_t pos) {
  (void) pos;
  size_t i = 0;
  int k;
  while (i + 32 <= n) {
    uint64_t r = xs64();
    for (k = 0; k < 32; k++) { b[i + k] = (uint8_t) dna_alpha[r & 3u];  r >>= 2; }
    i += 32;
  }
  if (i < n) {
    uint64_t r = xs64();
    while (i < n) { b[i++] = (uint8_t) dna_alpha[r & 3u]; r >>= 2; }
  }
}

static void gen_ascii(uint8_t * b, size_t n, uint64_t pos) {
  size_t i;
  (void) pos;
  for (i = 0; i < n; i++) {
    /*  Rejection-free: multiply 95 into the high bits and take them.  */
    uint64_t r = xs64();
    b[i] = (uint8_t) (0x20u + (unsigned) ((r * 95ULL) >> 32) % 95u);
  }
}

static void gen_biased(uint8_t * b, size_t n, uint64_t pos) {
  size_t i;
  (void) pos;
  for (i = 0; i < n; i++) {
    uint64_t r = xs64();
    b[i] = (r & 1u) ? (uint8_t) 0u : (uint8_t) (r >> 1);
  }
}

static void gen_sparse_bits(uint8_t * b, size_t n, uint64_t pos) {
  uint64_t k;
  (void) pos;
  memset(b, 0, n);
  /*  About 1.5% of bits set: n*8/64, roughly n/8 bits.  Repeats may
      collide so the actual density is slightly lower, which is fine.  */
  uint64_t total_bits = (uint64_t) n * 8ULL;
  uint64_t set_bits   = total_bits / 64ULL;
  for (k = 0; k < set_bits; k++) {
    uint64_t r = xs64();
    uint64_t bit = r % total_bits;
    b[bit >> 3] |= (uint8_t) (1u << (bit & 7u));
  }
}

/*  Persistent LCG state across chunk boundaries so output is one
    continuous stream.  */
static uint32_t lcg_state = 1u;
static void gen_lcg(uint8_t * b, size_t n, uint64_t pos) {
  size_t i;
  (void) pos;
  uint32_t s = lcg_state;
  for (i = 0; i < n; i++) {
    s = s * 1103515245u + 12345u;
    b[i] = (uint8_t) (s >> 16);
  }
  lcg_state = s;
}

static uint8_t walk_state = 0u;
static void gen_walk(uint8_t * b, size_t n, uint64_t pos) {
  size_t i;
  (void) pos;
  uint8_t v = walk_state;
  for (i = 0; i < n; i++) {
    int step = (int) ((xs64() & 7u)) - 3;  /*  -3..+4, mean ~0.5  */
    v = (uint8_t) ((int) v + step);
    b[i] = v;
  }
  walk_state = v;
}

static void gen_stripes(uint8_t * b, size_t n, uint64_t pos) {
  (void) pos;
  /*  8-byte stripes of one random byte: strong intra-block redundancy,
      uniform marginal byte distribution.  Tests SCC and MC Pi separately
      from raw byte entropy.  */
  size_t i = 0;
  while (i + 8 <= n) {
    uint8_t v = (uint8_t) xs64();
    memset(b + i, v, 8);
    i += 8;
  }
  while (i < n) b[i++] = (uint8_t) xs64();
}

static struct {
  const char * name;
  gen_fn fn;
} table[] = {
  { "random",      gen_random      },
  { "zeros",       gen_zeros       },
  { "ones",        gen_ones        },
  { "counter",     gen_counter     },
  { "dna",         gen_dna         },
  { "ascii",       gen_ascii       },
  { "biased",      gen_biased      },
  { "sparse-bits", gen_sparse_bits },
  { "lcg",         gen_lcg         },
  { "walk",        gen_walk        },
  { "stripes",     gen_stripes     },
};

int main(int argc, char ** argv) {
  size_t i;
  if (argc != 4) {
    fprintf(stderr, "usage: %s <kind> <size_MiB> <out-path>\n", argv[0]);
    fprintf(stderr, "kinds:");
    for (i = 0; i < sizeof (table)/sizeof (*table); i++)
      fprintf(stderr, " %s", table[i].name);
    fputc('\n', stderr);
    return 2;
  }
  const char * kind = argv[1];
  uint64_t mib = strtoull(argv[2], NULL, 10);
  const char * out = argv[3];

  gen_fn fn = NULL;
  for (i = 0; i < sizeof (table)/sizeof (*table); i++)
    if (!strcmp(table[i].name, kind)) { fn = table[i].fn; break; }
  if (!fn) { fprintf(stderr, "bench-gen: unknown kind '%s'\n", kind); return 2; }

  seed(kind);
  /*  Re-seed the persistent-state generators per kind.  */
  lcg_state = 1u + (uint32_t) xs64();
  walk_state = 0u;

  FILE * f = fopen(out, "wb");
  if (!f) { perror(out); return 1; }
  uint8_t * buf = (uint8_t *) malloc(CHUNK);
  if (!buf) { perror("malloc"); fclose(f); return 1; }

  uint64_t total = mib << 20;
  uint64_t pos = 0;
  while (pos < total) {
    size_t n = (size_t) ((total - pos) > CHUNK ? CHUNK : (total - pos));
    fn(buf, n, pos);
    if (fwrite(buf, 1, n, f) != n) { perror("write"); free(buf); fclose(f); return 1; }
    pos += n;
  }
  free(buf);
  if (fclose(f) != 0) { perror("fclose"); return 1; }
  return 0;
}
