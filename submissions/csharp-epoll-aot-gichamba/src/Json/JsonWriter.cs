using System.Buffers;
using System.Text;

namespace Feed.Json;

/// <summary>
/// UTF-8 JSON writer over a growable byte buffer. It inserts commas itself, except inside a value written piece by
/// piece with the Append methods, and escapes strings per RFC 8259; it does not check that calls form valid JSON.
/// </summary>
public sealed class JsonWriter {
    /// <summary>Milliseconds in a day.</summary>
    private const long MsPerDay = 86_400_000L;
    /// <summary>
    /// Days from 0000-03-01 to 2000-01-01, the Postgres epoch: civil_from_days counts from 1970-01-01 after adding
    /// 719,468, and 2000-01-01 is day 10,957 after 1970-01-01.
    /// </summary>
    private const long DaysFrom0000To2000 = 719_468L + 10_957L;

    /// <summary>"00" to "99" back to back, for writing two digits at once.</summary>
    private static ReadOnlySpan<byte> TwoDigits =>
        "00010203040506070809101112131415161718192021222324252627282930313233343536373839404142434445464748495051525354555657585960616263646566676869707172737475767778798081828384858687888990919293949596979899"u8;

    /// <summary>The bytes RFC 8259 requires escaped in a string: control characters, quote and backslash.</summary>
    private static readonly SearchValues<byte> MustEscape = SearchValues.Create(
        "\0\u0001\u0002\u0003\u0004\u0005\u0006\u0007\b\t\n\u000b\f\r\u000e\u000f\u0010\u0011\u0012\u0013\u0014\u0015\u0016\u0017\u0018\u0019\u001a\u001b\u001c\u001d\u001e\u001f\"\\"u8);

    /// <summary>The output buffer; replaced by a larger one when it fills.</summary>
    private byte[] _buf;
    /// <summary>Number of bytes written.</summary>
    private int _len;
    /// <summary>True before the first item of the current object or array, so no comma is needed.</summary>
    private bool _first = true;
    /// <summary>True right after a property name, so the next value takes no comma.</summary>
    private bool _afterName;

    /// <summary>Creates a writer with an initial buffer of <paramref name="capacity"/> bytes.</summary>
    public JsonWriter(int capacity = 4096) => _buf = new byte[capacity];

    /// <summary>The backing buffer; valid bytes end at <see cref="Length"/>. It may be replaced as the writer grows.</summary>
    public byte[] Buffer => _buf;

    /// <summary>The end of the written bytes (an index into <see cref="Buffer"/>).</summary>
    public int Length => _len;

    /// <summary>Forgets everything written and starts writing at <paramref name="start"/>, leaving that many bytes untouched in front.</summary>
    public void Reset(int start = 0) {
        Ensure(start);
        _len = start;
        _first = true;
        _afterName = false;
    }

    /// <summary>Writes '{'.</summary>
    public void StartObject() {
        BeforeValue();
        Put((byte)'{');
        _first = true;
    }

    /// <summary>Writes '}'.</summary>
    public void EndObject() {
        Put((byte)'}');
        _first = false;
    }

    /// <summary>Writes '['.</summary>
    public void StartArray() {
        BeforeValue();
        Put((byte)'[');
        _first = true;
    }

    /// <summary>Writes ']'.</summary>
    public void EndArray() {
        Put((byte)']');
        _first = false;
    }

    /// <summary>Writes an escaped property name and the colon.</summary>
    public void PropertyName(string name) {
        if (!_first) {
            Put((byte)',');
        }

        WriteQuoted(name);
        Put((byte)':');
        _afterName = true;
    }

    /// <summary>Writes an escaped property name given as UTF-8 bytes (a u8 literal needs no transcoding) and the colon.</summary>
    public void PropertyName(ReadOnlySpan<byte> utf8Name) {
        if (!_first) {
            Put((byte)',');
        }

        Put((byte)'"');
        EscapeUtf8(utf8Name);
        Put((byte)'"');
        Put((byte)':');
        _afterName = true;
    }

    /// <summary>Writes a string value, escaped.</summary>
    public void WriteString(string value) {
        BeforeValue();
        WriteQuoted(value);
    }

    /// <summary>Writes a string value given as UTF-8 bytes (for example a Postgres text column), escaped.</summary>
    public void WriteString(ReadOnlySpan<byte> utf8) {
        BeforeValue();
        Put((byte)'"');
        EscapeUtf8(utf8);
        Put((byte)'"');
    }

    /// <summary>Writes an integer value.</summary>
    public void WriteLong(long value) {
        BeforeValue();
        AppendLong(value);
    }

    /// <summary>
    /// Starts a value that the Append methods write piece by piece: writes the comma it needs and counts it as written.
    /// The caller then appends the whole value's JSON text, which this writer does not check.
    /// </summary>
    public void BeginRawValue() => BeforeValue();

    /// <summary>Appends bytes as they are: no comma and no escaping.</summary>
    public void AppendRaw(ReadOnlySpan<byte> json) => WriteRawBytes(json);

    /// <summary>Appends UTF-8 text escaped for the inside of a JSON string, without the quotes and without a comma.</summary>
    public void AppendEscaped(ReadOnlySpan<byte> utf8) => EscapeUtf8(utf8);

    /// <summary>Appends an integer without a comma.</summary>
    public void AppendLong(long value) {
        Ensure(_len + 20);
        value.TryFormat(_buf.AsSpan(_len), out int n, default, System.Globalization.CultureInfo.InvariantCulture);
        _len += n;
    }

    /// <summary>Writes true or false.</summary>
    public void WriteBool(bool value) {
        BeforeValue();
        WriteRawBytes(value ? "true"u8 : "false"u8);
    }

    /// <summary>
    /// Writes a quoted RFC 3339 UTC time with exactly three fraction digits from a Postgres
    /// timestamptz value (microseconds since 2000-01-01T00:00:00Z). Sub-millisecond digits are
    /// dropped by rounding toward negative infinity, so 23:59:59.9995 reads .999.
    /// </summary>
    public void WriteTimestamp(long pgMicros) {
        BeforeValue();
        AppendTimestamp(pgMicros);
    }

    /// <summary>Appends the quoted time of <see cref="WriteTimestamp"/> without a comma.</summary>
    public void AppendTimestamp(long pgMicros) {
        long ms = Math.DivRem(pgMicros, 1000, out long rem);
        if (rem < 0) {
            ms--;
        }

        long days = Math.DivRem(ms, MsPerDay, out long msLeft);
        if (msLeft < 0) {
            days--;
            msLeft += MsPerDay;
        }

        int msOfDay = (int)msLeft;
        // Year, month and day from the day number with integer arithmetic (Howard Hinnant's civil_from_days), on a
        // calendar whose years start on March 1 so that the leap day comes last.
        long z = days + DaysFrom0000To2000;
        long era = (z >= 0 ? z : z - 146_096) / 146_097;
        int doe = (int)(z - era * 146_097);
        int yoe = (doe - doe / 1460 + doe / 36_524 - doe / 146_096) / 365;
        int doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
        int mp = (5 * doy + 2) / 153;
        int day = doy - (153 * mp + 2) / 5 + 1;
        int month = mp < 10 ? mp + 3 : mp - 9;
        int year = (int)(yoe + era * 400) + (month <= 2 ? 1 : 0);

        Ensure(_len + 26);
        Span<byte> b = _buf.AsSpan(_len, 26);
        b[0] = (byte)'"';
        Pair(b, 1, year / 100);
        Pair(b, 3, year % 100);
        b[5] = (byte)'-';
        Pair(b, 6, month);
        b[8] = (byte)'-';
        Pair(b, 9, day);
        b[11] = (byte)'T';
        Pair(b, 12, msOfDay / 3_600_000);
        b[14] = (byte)':';
        Pair(b, 15, msOfDay / 60_000 % 60);
        b[17] = (byte)':';
        Pair(b, 18, msOfDay / 1000 % 60);
        b[20] = (byte)'.';
        int fraction = msOfDay % 1000;
        b[21] = (byte)('0' + fraction / 100);
        Pair(b, 22, fraction % 100);
        b[24] = (byte)'Z';
        b[25] = (byte)'"';
        _len += 26;
    }

    /// <summary>Writes <paramref name="value"/> (0 to 99) as two digits at <paramref name="at"/>.</summary>
    private static void Pair(Span<byte> b, int at, int value) {
        b[at] = TwoDigits[2 * value];
        b[at + 1] = TwoDigits[2 * value + 1];
    }

    /// <summary>Writes the comma a value needs, unless it is the first item or follows a property name.</summary>
    private void BeforeValue() {
        if (_afterName) {
            _afterName = false;
        } else if (!_first) {
            Put((byte)',');
        }

        _first = false;
    }

    /// <summary>Writes a quoted, escaped string. Plain ASCII without characters to escape goes straight into the buffer.</summary>
    private void WriteQuoted(string s) {
        // Property names and most values are plain ASCII: narrow them straight into the buffer.
        Ensure(_len + s.Length + 2);
        Span<byte> into = _buf.AsSpan(_len + 1, s.Length);
        if (Ascii.FromUtf16(s, into, out _) == OperationStatus.Done && into.IndexOfAny(MustEscape) < 0) {
            _buf[_len] = (byte)'"';
            _len += s.Length + 1;
            _buf[_len++] = (byte)'"';
            return;
        }

        Put((byte)'"');
        int max = Encoding.UTF8.GetMaxByteCount(s.Length);
        if (max <= 512) {
            Span<byte> tmp = stackalloc byte[512];
            int n = Encoding.UTF8.GetBytes(s, tmp);
            EscapeUtf8(tmp[..n]);
        } else {
            EscapeUtf8(Encoding.UTF8.GetBytes(s));
        }
        Put((byte)'"');
    }

    /// <summary>Writes UTF-8 bytes with JSON escapes; bytes that need no escape are copied unchanged.</summary>
    private void EscapeUtf8(ReadOnlySpan<byte> s) {
        Ensure(_len + s.Length + 8);
        while (true) {
            int i = s.IndexOfAny(MustEscape);
            if (i < 0) {
                WriteRawBytes(s);
                return;
            }

            WriteRawBytes(s[..i]);
            byte c = s[i];
            s = s[(i + 1)..];
            switch (c) {
                case (byte)'"': WriteRawBytes("\\\""u8); break;
                case (byte)'\\': WriteRawBytes("\\\\"u8); break;
                case (byte)'\n': WriteRawBytes("\\n"u8); break;
                case (byte)'\r': WriteRawBytes("\\r"u8); break;
                case (byte)'\t': WriteRawBytes("\\t"u8); break;
                case 8: WriteRawBytes("\\b"u8); break;
                case 12: WriteRawBytes("\\f"u8); break;
                default:
                    WriteRawBytes("\\u00"u8);
                    Put((byte)"0123456789abcdef"[c >> 4]);
                    Put((byte)"0123456789abcdef"[c & 15]);
                    break;
            }
        }
    }

    /// <summary>Appends bytes as they are.</summary>
    private void WriteRawBytes(ReadOnlySpan<byte> bytes) {
        Ensure(_len + bytes.Length);
        bytes.CopyTo(_buf.AsSpan(_len));
        _len += bytes.Length;
    }

    /// <summary>Appends one byte.</summary>
    private void Put(byte b) {
        if (_len == _buf.Length) {
            Ensure(_len + 1);
        }

        _buf[_len++] = b;
    }

    /// <summary>Grows the buffer, at least doubling it, until it holds <paramref name="needed"/> bytes.</summary>
    private void Ensure(int needed) {
        if (needed <= _buf.Length) {
            return;
        }

        int size = Math.Max(needed, _buf.Length * 2);
        Array.Resize(ref _buf, size);
    }
}
