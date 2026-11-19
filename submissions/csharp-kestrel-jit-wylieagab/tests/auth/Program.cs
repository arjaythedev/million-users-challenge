using System.Security.Cryptography;
using System.Text;
using MillionUsers;

byte[] key = Encoding.UTF8.GetBytes("million-users-challenge");
long now = DateTimeOffset.UtcNow.ToUnixTimeSeconds();
string goodClaims = $$"""{"sub":"1","username":"golden_ember_1","iat":{{now}},"exp":{{now + 3600}}}""";
string B64(byte[] value) => Convert.ToBase64String(value).TrimEnd('=').Replace('+', '-').Replace('/', '_');
string Sign(string payload, string header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}")
{
    string input = B64(Encoding.UTF8.GetBytes(header)) + "." + B64(Encoding.UTF8.GetBytes(payload));
    return input + "." + B64(HMACSHA256.HashData(key, Encoding.ASCII.GetBytes(input)));
}
int passed = 0;
void Check(string name, string? auth, string? expected, long id = 1, string username = "golden_ember_1")
{
    string? actual = Auth.Validate(auth, key, out long gotId, out string? gotName);
    if (actual != expected || (actual is null && (id != gotId || username != gotName)) ||
        (actual is not null && (gotId != 0 || gotName is not null)))
        throw new Exception($"{name}: expected {expected ?? "valid"}, got {actual ?? "valid"}, id={gotId}, username={gotName}");
    passed++;
}
void Claims(string name, string payload, string? expected, long id = 1, string username = "golden_ember_1") =>
    Check(name, "Bearer " + Sign(payload), expected, id, username);
const string Bad = "invalid or expired token", Payload = "invalid token payload", Missing = "missing bearer token";
string token = Sign(goodClaims);
Check("valid token", "Bearer " + token, null);
Check("missing header", null, Missing);
Check("Basic", "Basic " + token, Missing);
Check("case sensitive Bearer", "bearer " + token, Missing);
Check("empty token", "Bearer ", Bad);
Check("garbage token", "Bearer abc", Bad);
Check("trailing dot", "Bearer " + token + ".", Bad);
Check("leading whitespace", "Bearer  " + token, Bad);
Check("trailing whitespace", "Bearer " + token + " ", Bad);
Check("signature substitution", "Bearer " + token[..(token.LastIndexOf('.') + 1)] + new string('A', 43), Bad);
Check("none algorithm", "Bearer " + Sign(goodClaims, "{\"alg\":\"none\"}"), Bad);
Check("HS384 algorithm", "Bearer " + Sign(goodClaims, "{\"alg\":\"HS384\"}"), Bad);
Check("missing algorithm", "Bearer " + Sign(goodClaims, "{}"), Bad);
Check("duplicate algorithm", "Bearer " + Sign(goodClaims, "{\"alg\":\"HS256\",\"alg\":\"HS256\"}"), Bad);
Check("critical extension", "Bearer " + Sign(goodClaims, "{\"alg\":\"HS256\",\"crit\":[\"foo\"]}"), Bad);
Check("b64 false", "Bearer " + Sign(goodClaims, "{\"alg\":\"HS256\",\"b64\":false}"), Bad);
Check("header trailing document", "Bearer " + Sign(goodClaims, "{\"alg\":\"HS256\"}{}"), Bad);
Check("header array", "Bearer " + Sign(goodClaims, "[]"), Bad);
Check("escaped algorithm", "Bearer " + Sign(goodClaims, "{\"alg\":\"HS25\\u0036\"}"), null);
Claims("expired", goodClaims.Replace((now + 3600).ToString(), (now - 1).ToString()), Bad);
Claims("expiry now", goodClaims.Replace((now + 3600).ToString(), now.ToString()), Bad);
Claims("future fractional expiry", $$"""{"sub":"1","username":"golden_ember_1","exp":{{now + 3600}}.5}""", null);
Claims("absent expiry", "{\"sub\":\"1\",\"username\":\"x\"}", Bad);
Claims("string expiry", "{\"sub\":\"1\",\"username\":\"x\",\"exp\":\"9999999999\"}", Bad);
Claims("expiry overflow", "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1e999}", Bad);
Claims("duplicate expiry", goodClaims[..^1] + ",\"exp\":9999999999}", Bad);
Claims("future nbf", goodClaims[..^1] + ",\"nbf\":9999999999}", Bad);
Claims("past nbf", goodClaims[..^1] + ",\"nbf\":0}", null);
Claims("bad JSON", "{garbage}", Bad);
Claims("trailing JSON", goodClaims + "{}", Bad);
Claims("array payload", "[]", Bad);
Claims("nested unknown claim", goodClaims[..^1] + ",\"extra\":[1,{\"nested\":[true,false,null]}]}", null);
Claims("invalid sub alphabetic", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"abc\""), Payload);
Claims("invalid sub negative", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"-1\""), Payload);
Claims("invalid sub plus sign", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"+1\""), Payload);
Claims("invalid sub zero", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"0\""), Payload);
Claims("invalid sub empty", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"\""), Payload);
Claims("invalid sub numeric", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":1"), Payload);
Claims("invalid sub whitespace", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\" 1\""), Payload);
Claims("invalid sub fractional", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"1.5\""), Payload);
Claims("invalid sub overflow", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"9223372036854775808\""), Payload);
Claims("max long sub", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"9223372036854775807\""), null, long.MaxValue);
Claims("escaped sub", goodClaims.Replace("\"sub\":\"1\"", "\"sub\":\"\\u0031\""), null);
Claims("missing sub", goodClaims.Replace("\"sub\":\"1\",", ""), Payload);
Claims("duplicate sub", goodClaims[..^1] + ",\"sub\":\"2\"}", Payload);
Claims("wrong username type", goodClaims.Replace("\"username\":\"golden_ember_1\"", "\"username\":5"), Payload);
Claims("missing username", goodClaims.Replace("\"username\":\"golden_ember_1\",", ""), Payload);
Claims("duplicate username", goodClaims[..^1] + ",\"username\":\"other\"}", Payload);
Claims("unicode username", goodClaims.Replace("golden_ember_1", "caf\\u00e9 😀"), null, 1, "café 😀");
Claims("escaped surrogate pair", goodClaims.Replace("golden_ember_1", "\\uD83D\\uDE00"), null, 1, "😀");
Claims("lone high surrogate username", goodClaims.Replace("golden_ember_1", "\\uD800"), Bad);
Claims("lone low surrogate username", goodClaims.Replace("golden_ember_1", "\\uDC00"), Bad);
Claims("reversed surrogate pair", goodClaims.Replace("golden_ember_1", "\\uDC00\\uD800"), Bad);
Claims("escaped claim names", goodClaims.Replace("\"sub\"", "\"\\u0073ub\"").Replace("\"username\"", "\"\\u0075sername\"").Replace("\"exp\"", "\"e\\u0078p\""), null);
Claims("escaped duplicate sub name", goodClaims[..^1] + ",\"\\u0073ub\":\"2\"}", Payload);
Check("lone high surrogate algorithm", "Bearer " + Sign(goodClaims, "{\"alg\":\"\\uD800\"}"), Bad);
Claims("empty username string", goodClaims.Replace("golden_ember_1", ""), null, 1, "");
Claims("pooled large token", goodClaims[..^1] + ",\"padding\":\"" + new string('x', 2000) + "\"}", null);
Check("token cap", "Bearer " + new string('x', 32769), Bad);
// Deterministic malformed-input fuzzing must never throw or produce an identity.
var random = new Random(983);
for (int i = 0; i < 1000; i++)
{
    byte[] garbage = new byte[random.Next(0, 400)];
    random.NextBytes(garbage);
    Check("fuzz " + i, "Bearer " + Encoding.UTF8.GetString(garbage), Bad);
}
// Utf8JsonReader.Skip does not decode ignored string values; validate UTF-8 too.
byte[] malformedPayload = Encoding.UTF8.GetBytes(goodClaims[..^1] + ",\"extra\":\"?\"}");
malformedPayload[^3] = 0xff;
string malformedInput = B64(Encoding.UTF8.GetBytes("{\"alg\":\"HS256\"}")) + "." + B64(malformedPayload);
Check("invalid UTF8 in ignored claim", "Bearer " + malformedInput + "." +
    B64(HMACSHA256.HashData(key, Encoding.ASCII.GetBytes(malformedInput))), Bad);
Console.WriteLine($"Auth checks passed: {passed}");
