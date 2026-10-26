package feed

import java.io.ByteArrayOutputStream
import java.util.zip.{CRC32, Deflater}

/** Gzip for small, complete response bodies.
  *
  * http4s's `GZip` middleware streams every body through fs2's deflate pipe with fresh 32 KB buffers and chunked
  * encoding. These bodies are a few KB and already in memory, so one `Deflater` per thread (reset between uses)
  * compresses them in one call, and the response keeps its `Content-Length`. Level 1: most of the size win for a
  * fraction of the CPU.
  */
object Gzip:
  /** Bodies smaller than this go out as they are: they fit in one packet anyway. */
  val MinSize = 1024

  private val header = Array[Byte](0x1f, 0x8b.toByte, 8, 0, 0, 0, 0, 0, 0, 0xff.toByte)

  private val deflaters = ThreadLocal.withInitial(() => Deflater(1, true))

  def compress(bytes: Array[Byte]): Array[Byte] =
    val deflater = deflaters.get
    try
      deflater.setInput(bytes)
      deflater.finish()
      val out = ByteArrayOutputStream(bytes.length / 2 + 64)
      out.write(header)
      val buffer = new Array[Byte](4096)
      while !deflater.finished do out.write(buffer, 0, deflater.deflate(buffer))
      val crc = CRC32()
      crc.update(bytes)
      writeIntLE(out, crc.getValue.toInt)
      writeIntLE(out, bytes.length)
      out.toByteArray
    finally deflater.reset()

  private def writeIntLE(out: ByteArrayOutputStream, value: Int): Unit =
    out.write(value)
    out.write(value >>> 8)
    out.write(value >>> 16)
    out.write(value >>> 24)
