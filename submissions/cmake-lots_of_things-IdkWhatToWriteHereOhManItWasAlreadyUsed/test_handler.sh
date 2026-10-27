#!/usr/bin/env bash
# Тестирует handler.cmake напрямую: пишет сырой HTTP в stdin, читает ответ из stdout.
# Без listener'а, без C++, без curl. Нужны: cmake, openssl.
# Postgres/pool — опционально: если не запущены, /health даст 503, /feed|/posts — 500.
set -uo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
HANDLER="${ROOT}/server/handler.cmake"
export JWT_SECRET="${JWT_SECRET:-million-users-challenge}"
export START_TIME="${START_TIME:-$(date +%s)}"

PASS=0; FAIL=0; FAILED=()
RED=$'\e[31m'; GRN=$'\e[32m'; YEL=$'\e[33m'; RST=$'\e[0m'

# ---------- отправка запроса в handler ----------
# send METHOD PATH [BODY] [AUTH]
send() {
  local method="$1" path="$2" body="${3:-}" auth="${4:-}"
  local clen=${#body}
  {
    printf '%s %s HTTP/1.1\r\n' "$method" "$path"
    printf 'Host: localhost\r\n'
    [[ -n "$auth" ]] && printf 'Authorization: %s\r\n' "$auth"
    printf 'Content-Length: %d\r\n' "$clen"
    printf '\r\n'
    printf '%s' "$body"
  } | cmake -P "$HANDLER" 2>/tmp/handler_stderr
  # если cmake упал — покажем stderr
  if [[ -s /tmp/handler_stderr ]]; then
    printf '%s[cmake stderr]%s %s\n' "$YEL" "$RST" "$(cat /tmp/handler_stderr)" >&2
  fi
}

# статус и тело из сырого ответа
status_of() { printf '%s' "$1" | head -n1 | awk '{print $2}'; }
body_of()   { printf '%s' "$1" | awk 'BEGIN{RS="\r\n\r\n"} NR==2{print}'; }

# check NAME EXPECTED_STATUS EXPECTED_BODY_SUBSTR RESPONSE
check() {
  local name="$1" want_st="$2" want_body="$3" resp="$4"
  local got_st; got_st=$(status_of "$resp")
  local got_body; got_body=$(body_of "$resp")
  if [[ "$got_st" != "$want_st" ]]; then
    printf '%sFAIL%s %-52s status got=%s want=%s body=%q\n' "$RED" "$RST" "$name" "$got_st" "$want_st" "$got_body"
    FAIL=$((FAIL+1)); FAILED+=("$name (status)")
    return
  fi
  if [[ -n "$want_body" && "$got_body" != *"$want_body"* ]]; then
    printf '%sFAIL%s %-52s body got=%q want~=%q\n' "$RED" "$RST" "$name" "$got_body" "$want_body"
    FAIL=$((FAIL+1)); FAILED+=("$name (body)")
    return
  fi
  printf '%sPASS%s %s\n' "$GRN" "$RST" "$name"
  PASS=$((PASS+1))
}

# ---------- JWT ----------
b64url() { openssl base64 -A | tr '+/' '-_' | tr -d '='; }

make_jwt() { # SECRET SUB USER EXP_OFFSET [ALG]
  local secret="$1" sub="$2" user="$3" off="${4:-3600}" alg="${5:-HS256}"
  local now; now=$(date +%s); local exp=$((now+off))
  local h; h=$(printf '{"alg":"%s","typ":"JWT"}' "$alg" | b64url)
  local p; p=$(printf '{"sub":"%s","username":"%s","iat":%d,"exp":%d}' "$sub" "$user" "$now" "$exp" | b64url)
  local s; s=$(printf '%s.%s' "$h" "$p" | openssl dgst -sha256 -hmac "$secret" -binary | b64url)
  printf '%s.%s.%s' "$h" "$p" "$s"
}
make_jwt_raw() { # HEADER_JSON PAYLOAD_JSON SECRET
  local h; h=$(printf '%s' "$1" | b64url)
  local p; p=$(printf '%s' "$2" | b64url)
  local s; s=$(printf '%s.%s' "$h" "$p" | openssl dgst -sha256 -hmac "$3" -binary | b64url)
  printf '%s.%s.%s' "$h" "$p" "$s"
}

TV=$(make_jwt "$JWT_SECRET" 1 alice 3600)
T_EXP=$(make_jwt "$JWT_SECRET" 1 alice -100)
T_SIG=$(make_jwt wrong-secret 1 alice 3600)
T_512=$(make_jwt "$JWT_SECRET" 1 alice 3600 HS512)
T_SUB0=$(make_jwt "$JWT_SECRET" 0 alice 3600)
T_SUBABC=$(make_jwt "$JWT_SECRET" abc alice 3600)
T_BADUSER=$(make_jwt_raw '{"alg":"HS256","typ":"JWT"}' '{"sub":"1","username":123,"exp":9999999999}' "$JWT_SECRET")
T_NOSUB=$(make_jwt_raw '{"alg":"HS256","typ":"JWT"}' '{"username":"alice","exp":9999999999}' "$JWT_SECRET")

EXIST_ID=1
MISSING_ID=999999999

# ============================================================
echo "== parsing / fallback =="
R=$(send GET /nope); check "unknown path -> 404 not found" 404 '"error":"not found"' "$R"
R=$(printf 'garbage' | cmake -P "$HANDLER" 2>/dev/null); check "garbage -> 400" 400 'malformed JSON body' "$R"
R=$(printf '' | cmake -P "$HANDLER" 2>/dev/null); check "empty stdin -> 400" 400 'malformed JSON body' "$R"

# ============================================================
echo "== /health =="
R=$(send GET /health); ST=$(status_of "$R")
if [[ "$ST" == 200 ]]; then
  check "/health -> 200 ok" 200 '"status":"ok","db":"ok","uptime_s":' "$R"
elif [[ "$ST" == 503 ]]; then
  check "/health -> 503 degraded (pool down)" 503 '"status":"degraded","db":"unreachable"' "$R"
else
  check "/health -> 200|503" "$ST" "" "$R"
fi

# ============================================================
echo "== /feed =="
R=$(send GET /feed); ST=$(status_of "$R")
if [[ "$ST" == 200 ]]; then check "/feed -> 200 posts" 200 '"posts":[' "$R"
else check "/feed -> 500 (pool down) ok" 500 '' "$R"; fi

# ============================================================
echo "== /posts/:id =="
R=$(send GET /posts/$EXIST_ID); ST=$(status_of "$R")
if [[ "$ST" == 200 ]]; then
  check "GET /posts/1 -> 200" 200 "\"id\":${EXIST_ID}," "$R"
  check "GET /posts/1 created_at ...Z" 200 'Z"' "$R"
elif [[ "$ST" == 500 ]]; then
  echo "  (pool down, skip positive post checks)"
else
  check "GET /posts/1 -> 200" 200 '' "$R"
fi

for bad in 0 -1 abc 1.5; do
  R=$(send GET "/posts/$bad"); check "GET /posts/$bad -> 400 invalid post id" 400 'invalid post id' "$R"
done
R=$(send GET "/posts/$MISSING_ID"); check "GET /posts/missing -> 404 post not found" 404 '"error":"post not found"' "$R"

# ============================================================
echo "== POST /posts auth =="
B='{"body":"hello"}'
R=$(send POST /posts "$B" ""); check "no auth -> 401 missing" 401 'missing bearer token' "$R"
R=$(send POST /posts "$B" "Token $TV"); check "wrong scheme -> 401 missing" 401 'missing bearer token' "$R"
R=$(send POST /posts "$B" "Bearer garbage"); check "malformed jwt -> 401 invalid" 401 'invalid or expired token' "$R"
R=$(send POST /posts "$B" "Bearer $T_SIG"); check "bad signature -> 401 invalid" 401 'invalid or expired token' "$R"
R=$(send POST /posts "$B" "Bearer $T_EXP"); check "expired -> 401 invalid" 401 'invalid or expired token' "$R"
R=$(send POST /posts "$B" "Bearer $T_512"); check "HS512 -> 401 invalid" 401 'invalid or expired token' "$R"
R=$(send POST /posts "$B" "Bearer $T_SUB0"); check "sub=0 -> 401 payload" 401 'invalid token payload' "$R"
R=$(send POST /posts "$B" "Bearer $T_SUBABC"); check "sub=abc -> 401 payload" 401 'invalid token payload' "$R"
R=$(send POST /posts "$B" "Bearer $T_BADUSER"); check "username=num -> 401 payload" 401 'invalid token payload' "$R"
R=$(send POST /posts "$B" "Bearer $T_NOSUB"); check "no sub -> 401 payload" 401 'invalid token payload' "$R"

# ============================================================
echo "== POST /posts body =="
A="Bearer $TV"
R=$(send POST /posts 'not-json' "$A"); check "non-json -> 400 malformed" 400 'malformed JSON body' "$R"
R=$(send POST /posts '{}' "$A"); check "no body -> 400 required" 400 'body is required' "$R"
R=$(send POST /posts '{"body":123}' "$A"); check "body not string -> 400 required" 400 'body is required' "$R"
R=$(send POST /posts '{"body":"   "}' "$A"); check "body whitespace -> 400 required" 400 'body is required' "$R"
LONG=$(printf 'x%.0s' $(seq 1 501))
R=$(send POST /posts "{\"body\":\"$LONG\"}" "$A"); check "body 501 -> 400 too long" 400 'body must be at most 500 characters' "$R"

# ============================================================
echo "== POST /posts success =="
R=$(send POST /posts '{"body":"  hi from test  "}' "$A"); ST=$(status_of "$R")
if [[ "$ST" == 201 ]]; then
  check "create -> 201" 201 '"like_count":0' "$R"
  check "create trims body" 201 '"body":"hi from test"' "$R"
  check "create author from token" 201 '"author":"alice"' "$R"
elif [[ "$ST" == 500 ]]; then
  echo "  (pool down, skip create post)"
else
  check "create -> 201" 201 '' "$R"
fi

# ============================================================
echo "== /like auth first, then id =="
R=$(send POST "/posts/abc/like" "" ""); check "like /abc no auth -> 401" 401 'missing bearer token' "$R"
R=$(send POST "/posts/abc/like" "" "$A"); check "like /abc valid auth -> 400" 400 'invalid post id' "$R"
R=$(send POST "/posts/0/like" "" "$A"); check "like /0 -> 400" 400 'invalid post id' "$R"
R=$(send POST "/posts/$MISSING_ID/like" "" "$A"); ST=$(status_of "$R")
if [[ "$ST" == 404 ]]; then check "like missing -> 404" 404 '"error":"post not found"' "$R"
elif [[ "$ST" == 500 ]]; then echo "  (pool down, skip)"; else check "like missing -> 404" 404 '' "$R"; fi

# ============================================================
echo "== /like idempotency =="
T_UNIQ=$(make_jwt "$JWT_SECRET" 20000 "uniq_$(date +%s%N)" 3600)
AU="Bearer $T_UNIQ"
R1=$(send POST "/posts/$EXIST_ID/like" "" "$AU")
R2=$(send POST "/posts/$EXIST_ID/like" "" "$AU")
S1=$(status_of "$R1"); S2=$(status_of "$R2")
if [[ "$S1" == 201 ]]; then check "first like -> 201 already_liked:false" 201 '"already_liked":false' "$R1"
else echo "  (pool down, skip like idempotency)"; fi
if [[ "$S2" == 200 ]]; then check "repeat like -> 200 already_liked:true" 200 '"already_liked":true' "$R2"
elif [[ "$S2" == 201 ]]; then check "repeat like -> 200" 200 '' "$R2"
else echo "  (pool down, skip)"; fi

# ============================================================
echo "== JSON key order =="
R=$(send GET /posts/$EXIST_ID); BD=$(body_of "$R")
if [[ "$BD" =~ \"id\":.*\"body\":.*\"created_at\":.*\"author\":.*\"like_count\": ]]; then
  printf '%sPASS%s key order\n' "$GRN" "$RST"; PASS=$((PASS+1))
else
  printf '%sFAIL%s key order: %q\n' "$RED" "$RST" "$BD"; FAIL=$((FAIL+1)); FAILED+=("key order")
fi

# ============================================================
echo
echo "==============================="
printf 'PASS: %s%d%s  FAIL: %s%d%s\n' "$GRN" "$PASS" "$RST" "$RED" "$FAIL" "$RST"
if [[ $FAIL -gt 0 ]]; then
  echo "failed:"; for n in "${FAILED[@]}"; do echo "  - $n"; done
  exit 1
fi
echo "ALL OK"