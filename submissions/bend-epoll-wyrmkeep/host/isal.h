// The ISA-L arm of host/epoll.c's reply encoder (--gzip isal, docs/specs/S32-the-isal-arm.md).
// Outside every proof.
//
// gi_compress writes one gzip member (RFC 1952) with ISA-L's isal_deflate_stateless at level 0,
// the call the leaderboard's first place makes
// (vendor/million-users-challenge/submissions/c-epoll-unorig-username/server.c:97-101). At level
// 0 the library writes a feed as one dynamic block (RFC 1951 3.2.7, BTYPE 10) under a code table
// of its own, the same table for every body. host/gzip.h writes a fixed block (BTYPE 01). The
// arm exists so that a load run can give the generator the first place's stream
// (bench/servers/bend-epoll/README.md, "The reply encoder").
//
// The stream. struct isal_zstream is 82,384 bytes (its table of match heads, and a history
// buffer that a stateless call does not use), so the caller keeps one per process and none on
// the stack. A call reads nothing an earlier call left there: isal_deflate_stateless_init and
// the first lines of isal_deflate_stateless set every scalar the level-0 path reads, and the
// path clears the heads it can index before the first byte (igzip/igzip.c, reset_match_history
// with total_in 0). So no byte of one reply reaches another (A-NOCACHE,
// docs/01-challenge-contract.md). tests/host/gzip_test.mjs checks it per body: the stream that
// every earlier body went through and a stream filled with 0xA5 write the same bytes.
//
// The room. igzip_lib.h states no bound. The source does (igzip/igzip.c, isal_deflate_stateless,
// the same lines in 2.31.0, 2.31.1 and 2.32.1): when the compressed form does not fit, the call
// writes the input as stored blocks of at most 65,535 bytes, 5 bytes of header each, between the
// 10-byte gzip header and the 8-byte trailer, and it answers STATELESS_OVERFLOW only when the
// room is under that stored form's size. gi_bound(n) is at least that size, so with gi_bound(n)
// of room a call cannot fail.
#ifndef BM_ISAL_H
#define BM_ISAL_H

#include <stddef.h>
#include <stdint.h>
#include <isa-l/igzip_lib.h>

// The library counts input and output in 32 bits. A longer input gets 0: the caller sends it as
// it is. gi_bound(GI_MAX_IN) is under 2^32.
#define GI_MAX_IN (1u << 30)

// The room that gi_compress never fails with, for n bytes of at most GI_MAX_IN.
static size_t gi_bound(size_t n) {
  return n + 5 * (n / 65535 + 1) + 18;
}

// Writes src[0 .. n - 1] as one gzip member into dst[0 .. cap - 1]; answers its length, or 0 when
// n is over GI_MAX_IN or the library reports that the member does not fit (never with a cap of
// gi_bound(n) or more). s is the caller's stream, in any state.
static size_t gi_compress(struct isal_zstream* s, const uint8_t* src, size_t n, uint8_t* dst, size_t cap) {
  if (n > GI_MAX_IN) {
    return 0;
  }
  uint32_t room = cap > UINT32_MAX ? UINT32_MAX : (uint32_t)cap;
  isal_deflate_stateless_init(s);
  s->next_in   = (uint8_t*)src;  // the library reads it and does not write it
  s->avail_in  = (uint32_t)n;
  s->next_out  = dst;
  s->avail_out = room;
  s->gzip_flag = IGZIP_GZIP;
  s->level     = 0;
  return isal_deflate_stateless(s) == COMP_OK ? room - s->avail_out : 0;
}

#endif
