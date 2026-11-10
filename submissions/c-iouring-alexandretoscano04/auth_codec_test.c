#include "app.h"
#include <assert.h>
#include <limits.h>
#include <openssl/evp.h>
#include <openssl/hmac.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

static void base64(Buffer *b, const void *data, size_t len) {
    size_t cap = 4 * ((len + 2) / 3) + 1;
    unsigned char *text = malloc(cap);
    assert(text);
    int written = EVP_EncodeBlock(text, data, (int)len);
    while (written && text[written - 1] == '=') written--;
    for (int i = 0; i < written; i++) {
        if (text[i] == '+') text[i] = '-';
        if (text[i] == '/') text[i] = '_';
    }
    assert(buffer_append(b, text, (size_t)written));
    free(text);
}

static Buffer token(const char *header, const char *payload, const char *secret) {
    Buffer b = {0};
    base64(&b, header, strlen(header));
    assert(buffer_text(&b, "."));
    base64(&b, payload, strlen(payload));
    unsigned char digest[32];
    unsigned len = 0;
    assert(HMAC(EVP_sha256(), secret, (int)strlen(secret), (unsigned char *)b.data,
                b.len, digest, &len));
    assert(len == 32 && buffer_text(&b, "."));
    base64(&b, digest, len);
    return b;
}

static void verify(Auth *auth, const char *header, const char *payload, const char *secret,
                   const char *expected) {
    Buffer b = token(header, payload, secret);
    Request request = {0};
    const char *error = auth_verify(auth, b.data, b.len, &request);
    assert((!error && !expected) || (error && expected && !strcmp(error, expected)));
    if (!error) {
        assert(request.user_id == 42);
        assert(request.username_len == 6 && !memcmp(request.username, "name\0x", 6));
    }
    free(request.username);
    buffer_free(&b);
}

static void auth_tests(void) {
    Auth *auth = auth_create("secret");
    assert(auth);
    const char *header = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    const char *good = "{\"sub\":\"42\",\"username\":\"name\\u0000x\",\"exp\":4102444800}";
    for (int i = 0; i < 100; i++) verify(auth, header, good, "secret", NULL);
    verify(auth, header, good, "wrong", "invalid or expired token");
    verify(auth, "{\"alg\":\"none\"}", good, "secret", "invalid or expired token");
    verify(auth, "{\"alg\":\"HS256\",\"alg\":\"HS256\"}", good, "secret", "invalid or expired token");
    verify(auth, header, "{\"sub\":\"42\",\"username\":\"x\",\"exp\":1}", "secret", "invalid or expired token");
    verify(auth, header, "{\"sub\":\"42\",\"username\":\"x\"}", "secret", "invalid or expired token");
    verify(auth, header, "{\"sub\":\"42\",\"username\":\"x\",\"exp\":\"4102444800\"}", "secret", "invalid or expired token");
    verify(auth, header, "{\"sub\":\"42\",\"username\":\"x\",\"exp\":4102444800,\"nbf\":4102444799}", "secret", "invalid or expired token");
    verify(auth, header, "{\"sub\":42,\"username\":\"x\",\"exp\":4102444800}", "secret", "invalid token payload");
    verify(auth, header, "{\"sub\":\"0\",\"username\":\"x\",\"exp\":4102444800}", "secret", "invalid token payload");
    verify(auth, header, "{\"sub\":\" 42\",\"username\":\"x\",\"exp\":4102444800}", "secret", "invalid token payload");
    verify(auth, header, "{\"sub\":\"9223372036854775808\",\"username\":\"x\",\"exp\":4102444800}", "secret", "invalid token payload");
    verify(auth, header, "{\"sub\":\"42\",\"username\":1,\"exp\":4102444800}", "secret", "invalid token payload");
    verify(auth, header, "{\"sub\":\"42\",\"username\":\"\xff\",\"exp\":4102444800}", "secret", "invalid or expired token");
    Request request = {0};
    Buffer b = token(header, good, "secret");
    char last = b.data[b.len - 1];
    b.data[b.len - 1] = last == 'A' ? 'B' : (char)(last + 1);
    assert(auth_verify(auth, b.data, b.len, &request));
    assert(auth_verify(auth, "a.b.c.d", 7, &request));
    assert(auth_verify(auth, "a.b.c", 5, &request));
    buffer_free(&b);
    auth_destroy(auth);
}

static void body_check(const char *json, size_t len, const char *expected, const char *body) {
    Request request = {0};
    const char *error = parse_post_body(json, len, &request);
    assert((!error && !expected) || (error && expected && !strcmp(error, expected)));
    if (!error) assert(request.body_len == strlen(body) && !memcmp(request.body, body, strlen(body)));
    free(request.body);
}

static void body_tests(void) {
    body_check("{bad", 4, "malformed JSON body", NULL);
    body_check("{}", 2, "body is required", NULL);
    body_check("[]", 2, "body is required", NULL);
    body_check("{\"body\":2}", 10, "body is required", NULL);
    const char *white = "{\"body\":\" \\t\\r\\n\\u00a0\\u3000\"}";
    body_check(white, strlen(white), "body is required", NULL);
    const char *unicode = "{\"body\":\"\\u3000é 😀\\u2009\"}";
    body_check(unicode, strlen(unicode), NULL, "é 😀");
    const char *zero = "{\"body\":\"a\\u0000b\"}";
    body_check(zero, strlen(zero), "internal server error", NULL);
    const char *invalid = "{\"body\":\"\xc0\x80\"}";
    body_check(invalid, strlen(invalid), "malformed JSON body", NULL);
    const char *surrogate = "{\"body\":\"\\ud800\"}";
    body_check(surrogate, strlen(surrogate), "malformed JSON body", NULL);
    char repeated[2005];
    for (int i = 0; i < 501; i++) memcpy(repeated + i * 4, "😀", 4);
    repeated[2004] = 0;
    Buffer input = {0};
    assert(buffer_text(&input, "{\"body\":"));
    assert(buffer_json(&input, repeated, 2000));
    assert(buffer_text(&input, "}"));
    repeated[2000] = 0;
    body_check(input.data, input.len, NULL, repeated);
    input.len = 0;
    assert(buffer_text(&input, "{\"body\":"));
    repeated[2000] = (char)0xf0;
    assert(buffer_json(&input, repeated, 2004));
    assert(buffer_text(&input, "}"));
    body_check(input.data, input.len, "body must be at most 500 characters", NULL);
    buffer_free(&input);
}

static void write_i64(char data[8], int64_t value) {
    uint64_t n;
    memcpy(&n, &value, sizeof(n));
    for (unsigned i = 0; i < 8; i++) data[i] = (char)(n >> (56 - 8 * i));
}

static void stamp(int64_t microseconds, const char *expected) {
    Buffer b = {0};
    char binary[8];
    write_i64(binary, microseconds);
    assert(buffer_timestamp(&b, binary, 8));
    assert(!strcmp(b.data, expected));
    buffer_free(&b);
}

static void codec_tests(void) {
    stamp(0, "\"2000-01-01T00:00:00.000Z\"");
    stamp(-1, "\"1999-12-31T23:59:59.999Z\"");
    stamp(-1001, "\"1999-12-31T23:59:59.998Z\"");
    stamp(999999, "\"2000-01-01T00:00:00.999Z\"");
    stamp(-946684800000001LL, "\"1969-12-31T23:59:59.999Z\"");
    Buffer b = {0};
    char controls[] = {'\0', '\n', '\t', '"', '\\', 'x'};
    assert(buffer_json(&b, controls, sizeof(controls)));
    assert(!strcmp(b.data, "\"\\u0000\\u000a\\u0009\\\"\\\\x\""));
    buffer_free(&b);
    assert(buffer_integer(&b, INT64_MIN) && !strcmp(b.data, "-9223372036854775808"));
    buffer_free(&b);
    char id[8], date[8], count[4] = {0, 0, 1, 0};
    write_i64(id, 42);
    write_i64(date, -1);
    const char *values[5] = { id, "hello\nworld", date, "\xc3\xa9", count };
    int32_t lengths[5] = { 8, 11, 8, 2, 4 };
    assert(buffer_post_row(&b, values, lengths));
    assert(!strcmp(b.data, "{\"id\":42,\"body\":\"hello\\u000aworld\",\"created_at\":\"1999-12-31T23:59:59.999Z\",\"author\":\"\xc3\xa9\",\"like_count\":256}"));
    buffer_free(&b);
    Request *request = request_new();
    assert(request);
    request_error(request, 401, "invalid token payload");
    assert(request->status == 401 && !strcmp(request->response.data, "{\"error\":\"invalid token payload\"}"));
    request_destroy(request);
}

int main(void) {
    auth_tests();
    body_tests();
    codec_tests();
    puts("auth and codec tests passed");
    return 0;
}
