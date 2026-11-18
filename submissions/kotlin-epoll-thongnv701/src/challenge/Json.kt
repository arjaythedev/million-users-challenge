package challenge

import java.nio.ByteBuffer
import java.nio.charset.CharacterCodingException
import java.nio.charset.CharsetDecoder
import java.nio.charset.CodingErrorAction

/** JSON null. A missing key is Kotlin null, so the two stay apart. */
object JsonNull

/** Any malformed input. One preallocated instance without a stack trace. */
class Malformed private constructor() : Exception(null, null, false, false) {
    companion object {
        @JvmField val INSTANCE = Malformed()
    }
}

private const val MAX_DEPTH = 256

private val UTF8: ThreadLocal<CharsetDecoder> = ThreadLocal.withInitial {
    Charsets.UTF_8.newDecoder().onMalformedInput(CodingErrorAction.REPORT).onUnmappableCharacter(CodingErrorAction.REPORT)
}

/** Strict UTF-8: invalid bytes are malformed, not replaced. */
fun utf8(b: ByteArray, off: Int, len: Int): String {
    try {
        return UTF8.get().reset().decode(ByteBuffer.wrap(b, off, len)).toString()
    } catch (e: CharacterCodingException) {
        throw Malformed.INSTANCE
    }
}

fun jsonGet(v: Any?, key: String): Any? = if (v is Map<*, *>) v[key] else null

/** A strict JSON parser: objects become maps, arrays lists, numbers Doubles, null JsonNull. */
class Json private constructor(private val s: String) {
    private var i = 0

    companion object {
        fun parse(s: String): Any {
            val p = Json(s)
            p.ws()
            val v = p.value(0)
            p.ws()
            if (p.i != s.length) throw Malformed.INSTANCE
            return v
        }
    }

    private fun ws() {
        while (i < s.length) {
            val c = s[i]
            if (c != ' ' && c != '\t' && c != '\n' && c != '\r') return
            i++
        }
    }

    private fun value(depth: Int): Any {
        if (i >= s.length || depth > MAX_DEPTH) throw Malformed.INSTANCE
        val c = s[i]
        return when (c) {
            '{' -> obj(depth)
            '[' -> array(depth)
            '"' -> string()
            't' -> { literal("true"); true }
            'f' -> { literal("false"); false }
            'n' -> { literal("null"); JsonNull }
            else -> if (c == '-' || (c in '0'..'9')) number() else throw Malformed.INSTANCE
        }
    }

    private fun literal(word: String) {
        if (!s.startsWith(word, i)) throw Malformed.INSTANCE
        i += word.length
    }

    private fun obj(depth: Int): HashMap<String, Any> {
        i++
        val m = HashMap<String, Any>()
        ws()
        if (i < s.length && s[i] == '}') {
            i++
            return m
        }
        while (true) {
            ws()
            if (i >= s.length || s[i] != '"') throw Malformed.INSTANCE
            val key = string()
            ws()
            if (i >= s.length || s[i] != ':') throw Malformed.INSTANCE
            i++
            ws()
            m[key] = value(depth + 1)
            ws()
            if (i >= s.length) throw Malformed.INSTANCE
            val c = s[i++]
            if (c == '}') return m
            if (c != ',') throw Malformed.INSTANCE
        }
    }

    private fun array(depth: Int): ArrayList<Any> {
        i++
        val a = ArrayList<Any>()
        ws()
        if (i < s.length && s[i] == ']') {
            i++
            return a
        }
        while (true) {
            ws()
            a.add(value(depth + 1))
            ws()
            if (i >= s.length) throw Malformed.INSTANCE
            val c = s[i++]
            if (c == ']') return a
            if (c != ',') throw Malformed.INSTANCE
        }
    }

    private fun string(): String {
        i++
        val start = i
        while (i < s.length) {
            val c = s[i]
            if (c == '"') {
                val r = s.substring(start, i)
                i++
                return r
            }
            if (c == '\\') break
            if (c < ' ') throw Malformed.INSTANCE
            i++
        }
        val sb = StringBuilder(s.length - start)
        sb.append(s, start, i)
        while (i < s.length) {
            val c = s[i++]
            if (c == '"') return sb.toString()
            if (c < ' ') throw Malformed.INSTANCE
            if (c != '\\') {
                sb.append(c)
                continue
            }
            if (i >= s.length) throw Malformed.INSTANCE
            when (s[i++]) {
                '"' -> sb.append('"')
                '\\' -> sb.append('\\')
                '/' -> sb.append('/')
                'b' -> sb.append('\b')
                'f' -> sb.append('\u000c')
                'n' -> sb.append('\n')
                'r' -> sb.append('\r')
                't' -> sb.append('\t')
                'u' -> {
                    if (i + 4 > s.length) throw Malformed.INSTANCE
                    var v = 0
                    for (k in 0 until 4) {
                        val d = Character.digit(s[i++], 16)
                        if (d < 0) throw Malformed.INSTANCE
                        v = (v shl 4) or d
                    }
                    sb.append(v.toChar())
                }
                else -> throw Malformed.INSTANCE
            }
        }
        throw Malformed.INSTANCE
    }

    private fun number(): Double {
        val start = i
        if (s[i] == '-') i++
        if (i >= s.length) throw Malformed.INSTANCE
        if (s[i] == '0') {
            i++
        } else if (s[i] in '1'..'9') {
            while (i < s.length && isDigit(s[i])) i++
        } else {
            throw Malformed.INSTANCE
        }
        if (i < s.length && s[i] == '.') {
            i++
            if (i >= s.length || !isDigit(s[i])) throw Malformed.INSTANCE
            while (i < s.length && isDigit(s[i])) i++
        }
        if (i < s.length && (s[i] == 'e' || s[i] == 'E')) {
            i++
            if (i < s.length && (s[i] == '+' || s[i] == '-')) i++
            if (i >= s.length || !isDigit(s[i])) throw Malformed.INSTANCE
            while (i < s.length && isDigit(s[i])) i++
        }
        return s.substring(start, i).toDouble()
    }

    private fun isDigit(c: Char): Boolean = c in '0'..'9'
}
