//go:build isal

package main

/*
#include <stdlib.h>
#include <igzip_lib.h>

// Each thread keeps its own compression state. Every event loop is locked to
// its thread, so a loop always finds its own stream and never shares it.
static __thread struct isal_zstream *feed_stream;

// feed_gzip writes body as one complete gzip member (header, deflate data at
// ISA-L's level 0, CRC and size) to out and returns its length, or -1 if it
// did not fit. The Go pointers passed in are used only during the call.
static long feed_gzip(const unsigned char *in, unsigned int n, unsigned char *out, unsigned int room) {
	struct isal_zstream *s = feed_stream;
	if (s == NULL) {
		s = malloc(sizeof *s);
		if (s == NULL) return -1;
		feed_stream = s;
	}
	isal_deflate_stateless_init(s);
	s->next_in = (unsigned char *)in;
	s->avail_in = n;
	s->next_out = out;
	s->avail_out = room;
	s->gzip_flag = IGZIP_GZIP;
	s->level = 0;
	int status = isal_deflate_stateless(s);
	long written = (long)(room - s->avail_out);
	s->next_in = NULL;
	s->next_out = NULL;
	return status == COMP_OK ? written : -1;
}
*/
import "C"

import (
	"encoding/binary"
	"hash/crc32"
	"slices"
	"unsafe"
)

// gzipAppend appends a gzip member holding body to dst. ISA-L's level 0 is a
// hand-tuned assembly deflate with a fixed, general-purpose Huffman table:
// for responses of a few KiB it costs a fraction of what a portable encoder
// spends finding matches.
func gzipAppend(dst, body []byte) []byte {
	start := len(dst)
	// Room for incompressible input, which ISA-L writes as stored blocks.
	room := len(body) + len(body)/8 + 256
	dst = slices.Grow(dst, room)
	out := dst[start : start+room]
	var in *C.uchar
	if len(body) > 0 {
		in = (*C.uchar)(unsafe.Pointer(unsafe.SliceData(body)))
	}
	n := C.feed_gzip(in, C.uint(len(body)), (*C.uchar)(unsafe.Pointer(unsafe.SliceData(out))), C.uint(room))
	if n < 0 {
		return storedGzip(dst, body)
	}
	return dst[:start+int(n)]
}

// storedGzip writes body uncompressed, as stored deflate blocks.
func storedGzip(dst, body []byte) []byte {
	dst = append(dst, 0x1f, 0x8b, 8, 0, 0, 0, 0, 0, 0, 255)
	rest := body
	for {
		block := rest[:min(len(rest), 65535)]
		rest = rest[len(block):]
		final := byte(0)
		if len(rest) == 0 {
			final = 1
		}
		dst = append(dst, final)
		dst = binary.LittleEndian.AppendUint16(dst, uint16(len(block)))
		dst = binary.LittleEndian.AppendUint16(dst, ^uint16(len(block)))
		dst = append(dst, block...)
		if final == 1 {
			break
		}
	}
	dst = binary.LittleEndian.AppendUint32(dst, crc32.ChecksumIEEE(body))
	return binary.LittleEndian.AppendUint32(dst, uint32(len(body)))
}
