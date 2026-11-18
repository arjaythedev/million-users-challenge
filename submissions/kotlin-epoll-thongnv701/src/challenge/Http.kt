package challenge

/** Index just past the request head's blank line, or -1 while it is incomplete. */
fun headerEnd(b: ByteArray, p: Int, end: Int): Int {
    for (k in p + 3 until end) {
        if (b[k].toInt() == '\n'.code && b[k - 1].toInt() == '\r'.code && b[k - 2].toInt() == '\n'.code &&
            b[k - 3].toInt() == '\r'.code
        ) return k + 1
    }
    return -1
}

fun indexOfByte(b: ByteArray, x: Int, from: Int, to: Int): Int {
    for (k in from until to) if (b[k].toInt() == x) return k
    return -1
}

fun startsWith(b: ByteArray, at: Int, s: String): Boolean {
    for (k in 0 until s.length) if (b[at + k].toInt() != s[k].code) return false
    return true
}

fun endsWith(b: ByteArray, to: Int, s: String): Boolean = startsWith(b, to - s.length, s)

/** ASCII case-insensitive match against a lower-case name. */
fun nameIs(b: ByteArray, at: Int, lower: String): Boolean {
    for (k in 0 until lower.length) if ((b[at + k].toInt() or 0x20) != lower[k].code) return false
    return true
}

fun containsIgnoreCase(b: ByteArray, from: Int, to: Int, lower: String): Boolean {
    val n = lower.length
    var k = from
    while (k + n <= to) {
        if (nameIs(b, k, lower)) return true
        k++
    }
    return false
}

/** A positive decimal id; -1 if it is not one, -2 if it overflows a long (so no such post). */
fun parseId(b: ByteArray, from: Int, to: Int): Long {
    if (from >= to) return -1
    var v = 0L
    var overflow = false
    for (k in from until to) {
        val d = b[k].toInt() - '0'.code
        if (d < 0 || d > 9) return -1
        if (!overflow) {
            if (v > (Long.MAX_VALUE - d) / 10) overflow = true
            else v = v * 10 + d
        }
    }
    if (overflow) return -2
    return if (v > 0) v else -1
}

/** The whitespace JavaScript's String.prototype.trim removes. */
fun isJsSpace(c: Char): Boolean =
    c == ' ' || (c >= '\u0009' && c <= '\u000D') || c == ' ' || c == ' ' ||
        (c >= ' ' && c <= ' ') || c == ' ' || c == ' ' || c == ' ' ||
        c == ' ' || c == '　' || c == '﻿'

fun jsTrim(s: String): String {
    var a = 0
    var z = s.length
    while (a < z && isJsSpace(s[a])) a++
    while (z > a && isJsSpace(s[z - 1])) z--
    return s.substring(a, z)
}

/** A JSON string literal, for error messages. */
fun quote(s: String): String {
    val sb = StringBuilder(s.length + 2).append('"')
    for (c in s) {
        if (c == '"' || c == '\\') sb.append('\\').append(c)
        else if (c < ' ') sb.append(String.format("\\u%04x", c.code))
        else sb.append(c)
    }
    return sb.append('"').toString()
}
