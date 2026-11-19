#define _GNU_SOURCE
#include <time.h>
#include <assert.h>

static struct timespec claims_wall = {2000000000, 250000000};
static unsigned claims_wall_reads;

static int claims_clock(clockid_t clock, struct timespec *value) {
  if (clock == CLOCK_REALTIME) {
    *value = claims_wall;
    claims_wall_reads++;
    return 0;
  }
  return clock_gettime(clock, value);
}

#ifndef AUTH_CLAIMS_SERVER
#define AUTH_CLAIMS_SERVER "../server.c"
#endif
#define clock_gettime claims_clock
#define main claims_production_main
#include AUTH_CLAIMS_SERVER
#undef main
#undef clock_gettime

static const char claims_header[] = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
static const char claims_payload[] = "{\"sub\":\"7\",\"username\":\"alice\",\"exp\":2000000001}";
static const char claims_bad[] = "invalid or expired token";
static const char claims_invalid[] = "invalid token payload";
static const char claims_http_prefix[] = "POST /posts HTTP/1.1\r\nHost: fixture\r\nAuthorization: ";
static const char claims_http_suffix[] = "\r\nContent-Length: 1\r\n\r\n{";
static unsigned claims_checks, claims_failures, claims_baseline_observations;
static int claims_baseline;
static char claims_response[OBUF];
static char claims_name[MAXREQ + 64];
static char claims_token[MAXREQ * 2 + 128];

static void claims_check(const char *label, int matches) {
  claims_checks++;
  if (!matches) {
    fprintf(stderr, "FAIL %s\n", label);
    claims_failures++;
  }
}

static size_t claims_b64_length(size_t length) {
  return length / 3 * 4 + (length % 3 ? length % 3 + 1 : 0);
}

static size_t claims_b64(const unsigned char *input, size_t length, char *output) {
  static const char alphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
  size_t written = 0;
  for (size_t offset = 0; offset < length; offset += 3) {
    uint32_t value = (uint32_t)input[offset] << 16;
    if (offset + 1 < length) value |= (uint32_t)input[offset + 1] << 8;
    if (offset + 2 < length) value |= input[offset + 2];
    output[written++] = alphabet[value >> 18];
    output[written++] = alphabet[value >> 12 & 63];
    if (offset + 1 < length) output[written++] = alphabet[value >> 6 & 63];
    if (offset + 2 < length) output[written++] = alphabet[value & 63];
  }
  assert(written == claims_b64_length(length));
  output[written] = 0;
  return written;
}

static size_t claims_sign(const char *header, const char *payload) {
  unsigned char signature[32];
  size_t header_length = strlen(header), payload_length = strlen(payload);
  assert(7 + claims_b64_length(header_length) + 1 + claims_b64_length(payload_length) + 1 + 43 + 1 <= sizeof claims_token);
  memcpy(claims_token, "Bearer ", 7);
  size_t written = 7 + claims_b64((const unsigned char *)header, header_length, claims_token + 7);
  claims_token[written++] = '.';
  written += claims_b64((const unsigned char *)payload, payload_length, claims_token + written);
  hmac((const unsigned char *)claims_token + 7, written - 7, signature);
  claims_token[written++] = '.';
  written += claims_b64(signature, sizeof signature, claims_token + written);
  return written;
}

static size_t claims_wire_length(size_t token_length) {
  return sizeof claims_http_prefix - 1 + token_length + sizeof claims_http_suffix - 1;
}

static void claims_guard(void) {
  for (size_t offset = MAXREQ; offset < sizeof claims_name; offset++)
    assert((unsigned char)claims_name[offset] == 0xa5);
}

static void claims_route(const char *label, const char *path, const char *token, size_t token_length,
                         int status, const char *message) {
  th_t worker = {0};
  alen = 0;
  arena = claims_response;
  handle(&worker, 0, "POST", 4, path, strlen(path), token, token_length, "{", 1, 0);
  char expected_status[40], expected_body[160];
  int status_length = snprintf(expected_status, sizeof expected_status, "HTTP/1.1 %d ", status);
  int body_length = snprintf(expected_body, sizeof expected_body, "{\"error\":\"%s\"}", message);
  assert(status_length > 0 && (size_t)status_length < sizeof expected_status);
  assert(body_length > 0 && (size_t)body_length < sizeof expected_body);
  const char *body = memmem(arena, alen, "\r\n\r\n", 4);
  claims_check(label, alen >= (size_t)status_length && !memcmp(arena, expected_status, status_length) &&
               body && alen - (size_t)(body + 4 - arena) == (size_t)body_length &&
               !memcmp(body + 4, expected_body, body_length));
}

static void claims_accept(const char *label, const char *header, const char *payload, int64_t expected_uid,
                          const char *expected_name, size_t expected_length, const char *baseline_error) {
  size_t token_length = claims_sign(header, payload);
  assert(claims_wire_length(token_length) <= MAXREQ);
  memset(claims_name, 0xa5, sizeof claims_name);
  int64_t user_id = -1;
  size_t name_length = SIZE_MAX;
  const char *error = auth(claims_token, token_length, &user_id, claims_name, &name_length);
  claims_guard();
  if (claims_baseline && baseline_error) {
    claims_check(label, error && !strcmp(error, baseline_error));
    if (error && !strcmp(error, baseline_error)) {
      claims_baseline_observations++;
      printf("BASELINE REJECTION %s: %s\n", label, error);
    }
    claims_route(label, "/posts", claims_token, token_length, 401, baseline_error);
  } else {
    int matches = !error && user_id == expected_uid && name_length == expected_length;
    if (matches) matches = !memcmp(claims_name, expected_name, expected_length);
    claims_check(label, matches);
    if (error) fprintf(stderr, "  rejected: %s\n", error);
    claims_route(label, "/posts", claims_token, token_length, 400, "malformed JSON body");
  }
}

static void claims_reject(const char *label, const char *header, const char *payload, const char *expected) {
  size_t token_length = claims_sign(header, payload);
  assert(claims_wire_length(token_length) <= MAXREQ);
  memset(claims_name, 0xa5, sizeof claims_name);
  int64_t user_id = -1;
  size_t name_length = SIZE_MAX;
  const char *error = auth(claims_token, token_length, &user_id, claims_name, &name_length);
  claims_guard();
  claims_check(label, error && !strcmp(error, expected));
  claims_route(label, "/posts/abc/like", claims_token, token_length, 401, expected);
  claims_route(label, "/posts", claims_token, token_length, 401, expected);
}

static char *claims_pad(const char *document, size_t target) {
  size_t original = strlen(document);
  assert(original && document[original - 1] == '}' && target >= original + 9);
  char *padded = malloc(target + 1);
  assert(padded);
  memcpy(padded, document, original - 1);
  memcpy(padded + original - 1, ",\"pad\":\"", 8);
  memset(padded + original + 7, 'x', target - original - 9);
  memcpy(padded + target - 2, "\"}", 3);
  assert(strlen(padded) == target);
  return padded;
}

static char *claims_username(const char *unit, size_t repetitions) {
  static const char prefix[] = "{\"sub\":\"7\",\"username\":\"";
  static const char suffix[] = "\",\"exp\":2000000001}";
  size_t unit_length = strlen(unit), name_length = unit_length * repetitions;
  assert(!unit_length || name_length / unit_length == repetitions);
  char *payload = malloc(sizeof prefix - 1 + name_length + sizeof suffix);
  assert(payload);
  memcpy(payload, prefix, sizeof prefix - 1);
  for (size_t offset = 0; offset < repetitions; offset++)
    memcpy(payload + sizeof prefix - 1 + offset * unit_length, unit, unit_length);
  memcpy(payload + sizeof prefix - 1 + name_length, suffix, sizeof suffix);
  return payload;
}

static void claims_ids(void) {
  static const struct { const char *subject; int64_t value; const char *baseline_error; } accepted[] = {
    {"1", 1, NULL}, {"000000000000007", 7, NULL},
    {"1000000000000000", INT64_C(1000000000000000), claims_invalid},
    {"9007199254740991", INT64_C(9007199254740991), claims_invalid},
    {"9007199254740992", INT64_C(9007199254740992), claims_invalid},
    {"0000000000000007", 7, claims_invalid},
    {"0000000000000000000000000000000000000000000000000000000000000007", 7, claims_invalid},
    {"00009007199254740992", INT64_C(9007199254740992), claims_invalid}
  };
  for (size_t index = 0; index < sizeof accepted / sizeof accepted[0]; index++) {
    char payload[256], label[160];
    int written = snprintf(payload, sizeof payload, "{\"sub\":\"%s\",\"username\":\"alice\",\"exp\":2000000001}", accepted[index].subject);
    assert(written > 0 && (size_t)written < sizeof payload);
    snprintf(label, sizeof label, "positive decimal subject %s", accepted[index].subject);
    claims_accept(label, claims_header, payload, accepted[index].value, "alice", 5, accepted[index].baseline_error);
  }
  static const char *invalid[] = {"\"0\"", "\"000000000000000000000000000000000000\"", "\"-1\"", "\"1.5\"", "\"abc\"", "\"\"", "7", "null", "true", "[]", "{}"};
  for (size_t index = 0; index < sizeof invalid / sizeof invalid[0]; index++) {
    char payload[256], label[160];
    snprintf(payload, sizeof payload, "{\"sub\":%s,\"username\":\"alice\",\"exp\":2000000001}", invalid[index]);
    snprintf(label, sizeof label, "invalid subject %s", invalid[index]);
    claims_reject(label, claims_header, payload, claims_invalid);
  }
  claims_reject("missing subject", claims_header, "{\"username\":\"alice\",\"exp\":2000000001}", claims_invalid);
}

static void claims_names(void) {
  static const size_t lengths[] = {0, 1024, 1025, 1500, 8192, 48000};
  for (size_t index = 0; index < sizeof lengths / sizeof lengths[0]; index++) {
    size_t length = lengths[index];
    char *payload = claims_username("a", length), *expected = malloc(length + 1), label[96];
    assert(expected);
    memset(expected, 'a', length);
    snprintf(label, sizeof label, "username bytes %zu", length);
    const char *baseline_error = length > 1500 ? claims_bad : length > 1024 ? claims_invalid : NULL;
    claims_accept(label, claims_header, payload, 7, expected, length, baseline_error);
    free(expected);
    free(payload);
  }
  static const struct { const char *unit; const char *decoded; size_t length, repetitions; } escaped[] = {
    {"\\ud800", "\xef\xbf\xbd", 3, 1},
    {"\\udfff", "\xef\xbf\xbd", 3, 1},
    {"\\ud800", "\xef\xbf\xbd", 3, 350},
    {"\\ud800", "\xef\xbf\xbd", 3, 6000},
    {"\\udfff", "\xef\xbf\xbd", 3, 1200},
    {"\\ud83d\\ude00", "\xf0\x9f\x98\x80", 4, 400},
    {"\\u0000\\n\\\"\\\\", "\0\n\"\\", 4, 1},
    {"\xc3\xa9", "\xc3\xa9", 2, 600}
  };
  for (size_t index = 0; index < sizeof escaped / sizeof escaped[0]; index++) {
    size_t length = escaped[index].length * escaped[index].repetitions;
    char *payload = claims_username(escaped[index].unit, escaped[index].repetitions);
    char *expected = malloc(length + 1), label[96];
    assert(expected);
    for (size_t offset = 0; offset < escaped[index].repetitions; offset++)
      memcpy(expected + offset * escaped[index].length, escaped[index].decoded, escaped[index].length);
    snprintf(label, sizeof label, "escaped or UTF-8 username %zu (%zu bytes)", index, length);
    const char *baseline_error = claims_b64_length(strlen(payload)) > 2700 ? claims_bad : length > 1024 ? claims_invalid : NULL;
    claims_accept(label, claims_header, payload, 7, expected, length, baseline_error);
    free(expected);
    free(payload);
  }
  static const char *invalid[] = {"1", "null", "false", "[]", "{}"};
  for (size_t index = 0; index < sizeof invalid / sizeof invalid[0]; index++) {
    char payload[160], label[80];
    snprintf(payload, sizeof payload, "{\"sub\":\"7\",\"username\":%s,\"exp\":2000000001}", invalid[index]);
    snprintf(label, sizeof label, "nonstring username %s", invalid[index]);
    claims_reject(label, claims_header, payload, claims_invalid);
  }
  claims_reject("missing username", claims_header, "{\"sub\":\"7\",\"exp\":2000000001}", claims_invalid);
}

static void claims_segments(void) {
  static const size_t header_lengths[] = {510, 511, 512, 513, 4096};
  static const size_t payload_lengths[] = {2025, 2026, 2048, 2049, 8192};
  for (size_t index = 0; index < sizeof header_lengths / sizeof header_lengths[0]; index++) {
    char *header = claims_pad(claims_header, header_lengths[index]), label[96];
    snprintf(label, sizeof label, "header decoded %zu encoded %zu", header_lengths[index], claims_b64_length(header_lengths[index]));
    claims_accept(label, header, claims_payload, 7, "alice", 5, index ? claims_bad : NULL);
    free(header);
  }
  for (size_t index = 0; index < sizeof payload_lengths / sizeof payload_lengths[0]; index++) {
    char *payload = claims_pad(claims_payload, payload_lengths[index]), label[96];
    snprintf(label, sizeof label, "payload decoded %zu encoded %zu", payload_lengths[index], claims_b64_length(payload_lengths[index]));
    claims_accept(label, claims_header, payload, 7, "alice", 5, index ? claims_bad : NULL);
    free(payload);
  }
  char *header = claims_pad(claims_header, 4096), *payload = claims_pad(claims_payload, 8192);
  claims_accept("both segments beyond old caps", header, payload, 7, "alice", 5, claims_bad);
  free(header);
  free(payload);
  header = claims_pad("{\"alg\":\"HS384\"}", 4096);
  claims_reject("large signed wrong-algorithm header", header, claims_payload, claims_bad);
  free(header);
  payload = claims_pad("{\"sub\":7,\"username\":\"alice\",\"exp\":2000000001}", 8192);
  if (claims_baseline) claims_reject("large nonstring subject baseline cap", claims_header, payload, claims_bad);
  else claims_reject("large nonstring subject", claims_header, payload, claims_invalid);
  free(payload);
  for (int large_header = 0; large_header <= 1; large_header++) {
    size_t other = claims_b64_length(strlen(large_header ? claims_payload : claims_header));
    size_t target = MAXREQ / 4 * 3;
    while (claims_wire_length(7 + claims_b64_length(target) + 1 + other + 1 + 43) > MAXREQ) target--;
    char *document = claims_pad(large_header ? claims_header : claims_payload, target);
    size_t token_length = claims_sign(large_header ? document : claims_header, large_header ? claims_payload : document);
    assert(MAXREQ - claims_wire_length(token_length) <= 1);
    assert(claims_b64_length(target) > MAXREQ - 1024);
    claims_accept(large_header ? "MAXREQ-fitting header" : "MAXREQ-fitting payload", large_header ? document : claims_header,
                  large_header ? claims_payload : document, 7, "alice", 5, claims_bad);
    free(document);
  }
}

static void claims_security(void) {
  claims_reject("signed HS384 header", "{\"alg\":\"HS384\"}", claims_payload, claims_bad);
  claims_reject("signed none header", "{\"alg\":\"none\"}", claims_payload, claims_bad);
  claims_reject("numeric algorithm", "{\"alg\":256}", claims_payload, claims_bad);
  claims_reject("missing algorithm", "{}", claims_payload, claims_bad);
  claims_reject("expired", claims_header, "{\"sub\":\"7\",\"username\":\"alice\",\"exp\":1999999999}", claims_bad);
  claims_reject("expiry exactly now", claims_header, "{\"sub\":\"7\",\"username\":\"alice\",\"exp\":2000000000.25}", claims_bad);
  claims_reject("future nbf", claims_header, "{\"sub\":\"7\",\"username\":\"alice\",\"exp\":2000000001,\"nbf\":2000000000.5}", claims_bad);
  claims_reject("string expiry", claims_header, "{\"sub\":\"7\",\"username\":\"alice\",\"exp\":\"2000000001\"}", claims_bad);
  claims_reject("missing expiry", claims_header, "{\"sub\":\"7\",\"username\":\"alice\"}", claims_bad);
  claims_reject("malformed payload", claims_header, "{", claims_bad);
  claims_accept("noncanonical HS256 header", "{ \"typ\":\"JWT\", \"alg\":\"HS\\u0032\\u0035\\u0036\" }", claims_payload, 7, "alice", 5, NULL);
  size_t token_length = claims_sign(claims_header, claims_payload);
  claims_route("no bearer before bad path", "/posts/abc/like", NULL, 0, 401, "missing bearer token");
  claims_route("wrong bearer scheme before bad path", "/posts/abc/like", "Basic x", 7, 401, "missing bearer token");
  claims_route("valid auth before bad path", "/posts/abc/like", claims_token, token_length, 400, "invalid post id");
  claims_route("unknown route is not authenticated", "/missing", NULL, 0, 404, "not found");
  int64_t user_id = -1;
  size_t name_length = 0;
  char *signature = strrchr(claims_token, '.') + 1;
  signature[0] = signature[0] == 'A' ? 'B' : 'A';
  const char *error = auth(claims_token, token_length, &user_id, claims_name, &name_length);
  claims_check("tampered signature", error && !strcmp(error, claims_bad));
  claims_route("signature before path", "/posts/abc/like", claims_token, token_length, 401, claims_bad);
  token_length = claims_sign(claims_header, claims_payload);
  hmac_key("wrong-key");
  error = auth(claims_token, token_length, &user_id, claims_name, &name_length);
  claims_check("same token rechecks key", error && !strcmp(error, claims_bad));
  hmac_key("round54-test-secret");
  claims_wall.tv_sec += 2;
  error = auth(claims_token, token_length, &user_id, claims_name, &name_length);
  claims_check("same token rechecks expiry", error && !strcmp(error, claims_bad));
  claims_wall.tv_sec -= 2;
  now_s = 2100000000;
  error = auth(claims_token, token_length, &user_id, claims_name, &name_length);
  claims_check("stale future loop sample ignored", !error && user_id == 7 && name_length == 5 && !memcmp(claims_name, "alice", 5));
  now_s = 0;
}

int main(int argc, char **argv) {
  assert(argc == 1 || (argc == 2 && !strcmp(argv[1], "--expect-baseline")));
  claims_baseline = argc == 2;
  memset(b64t, -1, sizeof b64t);
  for (size_t index = 0; index < 64; index++)
    b64t[(unsigned char)"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_"[index]] = index;
  static const unsigned char expected_mac[32] = {
    0x5b,0xdc,0xc1,0x46,0xbf,0x60,0x75,0x4e,0x6a,0x04,0x24,0x26,0x08,0x95,0x75,0xc7,
    0x5a,0x00,0x3f,0x08,0x9d,0x27,0x39,0x83,0x9d,0xec,0x58,0xb9,0x64,0xec,0x38,0x43
  };
  unsigned char actual_mac[32];
  hmac_key("Jefe");
  static const char message[] = "what do ya want for nothing?";
  hmac((const unsigned char *)message, sizeof message - 1, actual_mac);
  claims_check("RFC 4231 case 2 HMAC-SHA256 anchor", !memcmp(actual_mac, expected_mac, sizeof actual_mac));
  hmac_key("round54-test-secret");
  now_s = 0;
  arena = claims_response;
  claims_ids();
  claims_names();
  claims_segments();
  claims_security();
  claims_check("fresh realtime sampled", claims_wall_reads > 0);
  if (claims_baseline) claims_check("baseline failures actually reproduced", claims_baseline_observations > 0);
  printf("%s: %u checks, %u failures, %u documented baseline rejections\n",
         claims_baseline ? "BASELINE EVIDENCE ONLY" : "AUTH CLAIMS CONTRACT", claims_checks, claims_failures, claims_baseline_observations);
  return claims_failures ? 1 : 0;
}
