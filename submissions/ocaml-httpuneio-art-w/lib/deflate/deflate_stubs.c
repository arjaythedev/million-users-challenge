#include <libdeflate.h>
#include <caml/mlvalues.h>
#include <caml/alloc.h>
#include <caml/custom.h>
#include <caml/fail.h>

#define Compressor_val(v) (*((struct libdeflate_compressor **)Data_custom_val(v)))

static void finalize_compressor(value v)
{
  if (Compressor_val(v) != NULL) libdeflate_free_compressor(Compressor_val(v));
}

static struct custom_operations compressor_ops = {
  "deflate.compressor", finalize_compressor,
  custom_compare_default, custom_hash_default, custom_serialize_default,
  custom_deserialize_default, custom_compare_ext_default, custom_fixed_length_default
};

value ld_alloc_compressor(value level)
{
  struct libdeflate_compressor *c = libdeflate_alloc_compressor(Int_val(level));
  if (c == NULL) caml_failwith("libdeflate_alloc_compressor");
  value v = caml_alloc_custom(&compressor_ops, sizeof(c), 0, 1);
  Compressor_val(v) = c;
  return v;
}

value ld_gzip_bound(value c, value len)
{
  return Val_long(libdeflate_gzip_compress_bound(Compressor_val(c), Long_val(len)));
}

value ld_gzip(value c, value src, value dst)
{
  return Val_long(libdeflate_gzip_compress(Compressor_val(c), String_val(src), caml_string_length(src),
                                           Bytes_val(dst), caml_string_length(dst)));
}
