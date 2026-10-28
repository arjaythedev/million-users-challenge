module Feed
  @[Link("deflate")]
  lib Deflate
    fun alloc = libdeflate_alloc_compressor(level : Int32) : Void*
    fun compress = libdeflate_gzip_compress(compressor : Void*, input : Void*, size : LibC::SizeT, output : Void*, capacity : LibC::SizeT) : LibC::SizeT
    fun bound = libdeflate_gzip_compress_bound(compressor : Void*, size : LibC::SizeT) : LibC::SizeT
  end

  module Gzip
    COMPRESSOR = Deflate.alloc(6)
    BUFFER     = Rayo::Buf.new(16384)

    def self.apply(ctx) : Nil
      return if ctx.output.len < 1024
      accept = ctx.header("Accept-Encoding")
      return unless accepts?(accept)
      raise "libdeflate allocation failed" if COMPRESSOR.null?
      size = ctx.output.len
      BUFFER.clear
      BUFFER.reserve(Deflate.bound(COMPRESSOR, size).to_i32)
      n = Deflate.compress(COMPRESSOR, ctx.output.ptr, size, BUFFER.ptr, BUFFER.cap).to_i32
      if n > 0 && n < size
        ctx.output.clear
        ctx.write(Bytes.new(BUFFER.ptr, n))
        ctx.header "Content-Encoding", "gzip"
        ctx.header "Vary", "Accept-Encoding"
      end
    end

    # Honor explicit q=0; no String splitting on the request path.
    def self.accepts?(value : Bytes) : Bool
      value.each_with_index do |c, i|
        if c == 103 && i + 4 <= value.size && value[i, 4] == "gzip".to_slice
          tail = value[i + 4..]
          last = tail.index(','.ord.to_u8) || tail.size
          part = tail[0, last]
          if q = part.index('='.ord.to_u8)
            return part[q + 1..].any? { |digit| 49 <= digit <= 57 }
          end
          return true
        end
      end
      false
    end
  end
end
