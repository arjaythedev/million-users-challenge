#include "app.h"
#include "yyjson.h"
#include <limits.h>
#include <math.h>
#include <openssl/core_names.h>
#include <openssl/crypto.h>
#include <openssl/evp.h>
#include <openssl/params.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

struct Auth {
    EVP_MAC *algorithm;
    EVP_MAC_CTX *context;
    Buffer header, payload;
};

static const char bad_token[] = "invalid or expired token";
static const char bad_payload[] = "invalid token payload";
static const char internal_error[] = "internal server error";

Auth *auth_create(const char *secret) {
    if (!secret || !*secret) return NULL;
    Auth *auth = calloc(1, sizeof(*auth));
    if (!auth) return NULL;
    auth->algorithm = EVP_MAC_fetch(NULL, "HMAC", NULL);
    if (auth->algorithm) auth->context = EVP_MAC_CTX_new(auth->algorithm);
    char digest[] = "SHA256";
    OSSL_PARAM params[] = {
        OSSL_PARAM_construct_utf8_string(OSSL_MAC_PARAM_DIGEST, digest, 0),
        OSSL_PARAM_construct_end()
    };
    if (!auth->context || EVP_MAC_init(auth->context, (const unsigned char *)secret,
                                       strlen(secret), params) != 1) {
        auth_destroy(auth);
        return NULL;
    }
    return auth;
}

void auth_destroy(Auth *auth) {
    if (!auth) return;
    EVP_MAC_CTX_free(auth->context);
    EVP_MAC_free(auth->algorithm);
    buffer_free(&auth->header);
    buffer_free(&auth->payload);
    free(auth);
}

static int base64_value(unsigned char c) {
    if (c >= 'A' && c <= 'Z') return c - 'A';
    if (c >= 'a' && c <= 'z') return c - 'a' + 26;
    if (c >= '0' && c <= '9') return c - '0' + 52;
    if (c == '-') return 62;
    if (c == '_') return 63;
    return -1;
}

static bool base64_decode(const char *source, size_t len, Buffer *target) {
    if (!len || len % 4 == 1 || len > MAX_REQUEST) return false;
    target->len = 0;
    size_t required = (len / 4) * 3 + 3 + YYJSON_PADDING_SIZE;
    if (!buffer_append(target, "", 0)) return false;
    if (required > target->cap) {
        char *data = realloc(target->data, required);
        if (!data) return false;
        target->data = data;
        target->cap = required;
    }
    unsigned acc = 0, bits = 0;
    for (size_t i = 0; i < len; i++) {
        int digit = base64_value((unsigned char)source[i]);
        if (digit < 0) return false;
        acc = (acc << 6) | (unsigned)digit;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            target->data[target->len++] = (char)(acc >> bits);
        }
    }
    if (bits && (acc & ((1u << bits) - 1u))) return false;
    memset(target->data + target->len, 0, YYJSON_PADDING_SIZE);
    return true;
}

static bool unique_claims(yyjson_val *object, bool header) {
    unsigned seen = 0;
    size_t index, count;
    yyjson_val *key, *value;
    yyjson_obj_foreach(object, index, count, key, value) {
        (void)value;
        unsigned bit = 0;
        if (header) {
            if (yyjson_equals_str(key, "alg")) bit = 1;
        } else {
            if (yyjson_equals_str(key, "sub")) bit = 1;
            else if (yyjson_equals_str(key, "username")) bit = 2;
            else if (yyjson_equals_str(key, "exp")) bit = 4;
            else if (yyjson_equals_str(key, "nbf")) bit = 8;
        }
        if (seen & bit) return false;
        seen |= bit;
    }
    return true;
}

static bool positive_id(yyjson_val *value, int64_t *id) {
    if (!yyjson_is_str(value)) return false;
    const char *text = yyjson_get_str(value);
    size_t len = yyjson_get_len(value);
    if (!len) return false;
    uint64_t n = 0;
    for (size_t i = 0; i < len; i++) {
        unsigned digit = (unsigned char)text[i] - (unsigned)'0';
        if (digit > 9 || n > ((uint64_t)INT64_MAX - digit) / 10) return false;
        n = n * 10 + digit;
    }
    if (!n) return false;
    *id = (int64_t)n;
    return true;
}

const char *auth_verify(Auth *auth, const char *token, size_t len, Request *request) {
    if (!auth || !token || !len || len > MAX_REQUEST) return bad_token;
    const char *first = memchr(token, '.', len);
    if (!first) return bad_token;
    size_t remainder = len - (size_t)(first + 1 - token);
    const char *second = memchr(first + 1, '.', remainder);
    if (!second || memchr(second + 1, '.', len - (size_t)(second + 1 - token))) return bad_token;
    size_t head_len = (size_t)(first - token), payload_len = (size_t)(second - first - 1);
    size_t signature_len = len - (size_t)(second + 1 - token);
    if (signature_len != 43 || !base64_decode(token, head_len, &auth->header) ||
        !base64_decode(first + 1, payload_len, &auth->payload)) return bad_token;
    char signature_bytes[64];
    Buffer signature = { .data = signature_bytes, .cap = sizeof(signature_bytes) };
    if (!base64_decode(second + 1, signature_len, &signature) || signature.len != 32) return bad_token;
    unsigned char computed[32];
    size_t computed_len = 0;
    if (EVP_MAC_init(auth->context, NULL, 0, NULL) != 1 ||
        EVP_MAC_update(auth->context, (const unsigned char *)token, (size_t)(second - token)) != 1 ||
        EVP_MAC_final(auth->context, computed, &computed_len, sizeof(computed)) != 1) return internal_error;
    if (computed_len != sizeof(computed) || CRYPTO_memcmp(computed, signature.data, sizeof(computed))) return bad_token;
    yyjson_doc *header = yyjson_read_opts(auth->header.data, auth->header.len, YYJSON_READ_INSITU, NULL, NULL);
    if (!header) return bad_token;
    yyjson_val *root = yyjson_doc_get_root(header);
    bool header_ok = yyjson_is_obj(root) && unique_claims(root, true) &&
                     yyjson_equals_str(yyjson_obj_get(root, "alg"), "HS256");
    yyjson_doc_free(header);
    if (!header_ok) return bad_token;
    yyjson_doc *payload = yyjson_read_opts(auth->payload.data, auth->payload.len, YYJSON_READ_INSITU, NULL, NULL);
    if (!payload) return bad_token;
    root = yyjson_doc_get_root(payload);
    const char *error = bad_token;
    if (!yyjson_is_obj(root) || !unique_claims(root, false)) goto done;
    yyjson_val *exp = yyjson_obj_get(root, "exp"), *nbf = yyjson_obj_get(root, "nbf");
    double now = (double)time(NULL), expires = yyjson_get_num(exp), begins = yyjson_get_num(nbf);
    if (!yyjson_is_num(exp) || !isfinite(expires) || expires <= now ||
        (nbf && (!yyjson_is_num(nbf) || !isfinite(begins) || begins > now))) goto done;
    int64_t user_id;
    yyjson_val *name = yyjson_obj_get(root, "username");
    if (!positive_id(yyjson_obj_get(root, "sub"), &user_id) || !yyjson_is_str(name)) {
        error = bad_payload;
        goto done;
    }
    size_t name_len = yyjson_get_len(name);
    char *username = malloc(name_len + 1);
    if (!username) { error = internal_error; goto done; }
    memcpy(username, yyjson_get_str(name), name_len);
    username[name_len] = 0;
    free(request->username);
    request->user_id = user_id;
    request->username = username;
    request->username_len = name_len;
    error = NULL;
done:
    yyjson_doc_free(payload);
    return error;
}

static uint32_t utf8_next(const unsigned char **cursor) {
    const unsigned char *text = *cursor;
    uint32_t c = *text++;
    if (c >= 0xf0) { c &= 7; c = (c << 6) | (*text++ & 63); }
    else if (c >= 0xe0) c &= 15;
    else if (c >= 0xc0) c &= 31;
    else { *cursor = text; return c; }
    c = (c << 6) | (*text++ & 63);
    if ((**cursor) >= 0xe0) c = (c << 6) | (*text++ & 63);
    *cursor = text;
    return c;
}

static bool whitespace(uint32_t c) {
    return (c >= 9 && c <= 13) || c == 32 || c == 0x85 || c == 0xa0 ||
           c == 0x1680 || (c >= 0x2000 && c <= 0x200a) || c == 0x2028 ||
           c == 0x2029 || c == 0x202f || c == 0x205f || c == 0x3000 || c == 0xfeff;
}

const char *parse_post_body(const char *body, size_t len, Request *request) {
    yyjson_doc *doc = yyjson_read(body, len, 0);
    if (!doc) return "malformed JSON body";
    yyjson_val *root = yyjson_doc_get_root(doc);
    yyjson_val *value = yyjson_is_obj(root) ? yyjson_obj_get(root, "body") : NULL;
    const char *error = "body is required";
    if (!yyjson_is_str(value)) goto done;
    const unsigned char *text = (const unsigned char *)yyjson_get_str(value);
    const unsigned char *end = text + yyjson_get_len(value), *start = text, *trim_end = text;
    size_t count = 0, trimmed_count = 0;
    while (text < end) {
        uint32_t scalar = utf8_next(&text);
        if (count == 0 && whitespace(scalar)) { start = text; continue; }
        count++;
        if (!whitespace(scalar)) { trim_end = text; trimmed_count = count; }
    }
    if (!trimmed_count) goto done;
    if (trimmed_count > 500) { error = "body must be at most 500 characters"; goto done; }
    size_t trimmed_len = (size_t)(trim_end - start);
    if (memchr(start, 0, trimmed_len)) { error = internal_error; goto done; }
    char *copy = malloc(trimmed_len + 1);
    if (!copy) { error = internal_error; goto done; }
    memcpy(copy, start, trimmed_len);
    copy[trimmed_len] = 0;
    free(request->body);
    request->body = copy;
    request->body_len = trimmed_len;
    error = NULL;
done:
    yyjson_doc_free(doc);
    return error;
}
