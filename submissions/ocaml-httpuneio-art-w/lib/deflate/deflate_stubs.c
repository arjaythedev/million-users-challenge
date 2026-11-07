#include <stdint.h>
#include <stdlib.h>
#include <isa-l/igzip_lib.h>
#include <caml/mlvalues.h>
#include <caml/alloc.h>
#include <caml/custom.h>
#include <caml/fail.h>

/* A compressor is igzip's level-0 work buffer (hash table), reused across calls. */
#define Level_buf(v) (*((uint8_t **)Data_custom_val(v)))

static void finalize_compressor(value v)
{
  free(Level_buf(v));
}

static struct custom_operations compressor_ops = {
  "deflate.compressor", finalize_compressor,
  custom_compare_default, custom_hash_default, custom_serialize_default,
  custom_deserialize_default, custom_compare_ext_default, custom_fixed_length_default
};

value igz_alloc_compressor(value unit)
{
  (void)unit;
  uint8_t *buf = malloc(ISAL_DEF_LVL0_DEFAULT);
  if (buf == NULL) caml_failwith("Deflate.compressor");
  value v = caml_alloc_custom(&compressor_ops, sizeof(buf), 0, 1);
  Level_buf(v) = buf;
  return v;
}

/* Compresses all of [src] into [dst] as one gzip member, at igzip's level 0 (one pass, one
   hash lookup per position). Returns the compressed length, or -1 if [dst] is too small. */
value igz_gzip(value c, value src, value dst)
{
  struct isal_zstream z;
  isal_deflate_stateless_init(&z);
  z.level = 0;
  z.level_buf = Level_buf(c);
  z.level_buf_size = ISAL_DEF_LVL0_DEFAULT;
  z.gzip_flag = IGZIP_GZIP;
  z.end_of_stream = 1;
  z.flush = NO_FLUSH;
  z.next_in = (uint8_t *)String_val(src);
  z.avail_in = caml_string_length(src);
  z.next_out = Bytes_val(dst);
  z.avail_out = caml_string_length(dst);
  if (isal_deflate_stateless(&z) != COMP_OK) return Val_long(-1);
  return Val_long(z.total_out);
}
