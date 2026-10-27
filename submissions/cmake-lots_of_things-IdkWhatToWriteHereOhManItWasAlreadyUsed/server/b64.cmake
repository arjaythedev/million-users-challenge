# server/b64.cmake
# base64url <-> base64, decode через openssl, HMAC-SHA256 -> base64url.
# Все вызовы openssl — через INPUT_FILE / OUTPUT_FILE, без stdin-пайпов,
# чтобы не висело на tty и не теряло \0 в CMake-строках.

# ---- base64url -> обычный base64 (с паддингом) ----
function(b64url_to_b64 IN OUT OK)
  string(REPLACE "-" "+" T "${IN}")
  string(REPLACE "_" "/" T "${T}")
  string(LENGTH "${T}" L)
  math(EXPR REM "${L} % 4")
  if(REM EQUAL 1)
    set(${OUT} "" PARENT_SCOPE)
    set(${OK} FALSE PARENT_SCOPE)
    return()
  elseif(REM EQUAL 2)
    set(T "${T}==")
  elseif(REM EQUAL 3)
    set(T "${T}=")
  endif()
  set(${OUT} "${T}" PARENT_SCOPE)
  set(${OK} TRUE PARENT_SCOPE)
endfunction()

# ---- обычный base64 -> base64url (без паддинга) ----
function(b64_to_b64url IN OUT)
  string(REPLACE "+" "-" T "${IN}")
  string(REPLACE "/" "_" T "${T}")
  string(REGEX REPLACE "=+$" "" T "${T}")
  string(STRIP "${T}" T)
  set(${OUT} "${T}" PARENT_SCOPE)
endfunction()

# ---- base64url decode -> сырые байты, OK=TRUE/FALSE ----
function(b64url_decode IN OUT OK)
  if(IN STREQUAL "")
    set(${OUT} "" PARENT_SCOPE)
    set(${OK} FALSE PARENT_SCOPE)
    return()
  endif()
  b64url_to_b64("${IN}" B64 B64_OK)
  if(NOT B64_OK)
    set(${OUT} "" PARENT_SCOPE)
    set(${OK} FALSE PARENT_SCOPE)
    return()
  endif()

  string(RANDOM LENGTH 16 _RND)
  set(_f "/tmp/_b64d_${_RND}")
  file(WRITE "${_f}" "${B64}")

  execute_process(
    COMMAND openssl base64 -d -A -in "${_f}"
    OUTPUT_VARIABLE DEC
    ERROR_QUIET
    RESULT_VARIABLE RC
    INPUT_FILE /dev/null
  )
  file(REMOVE "${_f}")

  if(NOT RC EQUAL 0)
    set(${OUT} "" PARENT_SCOPE)
    set(${OK} FALSE PARENT_SCOPE)
    return()
  endif()
  set(${OUT} "${DEC}" PARENT_SCOPE)
  set(${OK} TRUE PARENT_SCOPE)
endfunction()

# ---- HMAC-SHA256(DATA, SECRET) -> base64url ----
# INPUT_FILE/OUTPUT_FILE: openssl читает/пишет файлы, не stdin/stdout,
# поэтому не висит на tty и не теряет нулевые байты в CMake-строках.
function(hmac_sha256_b64url DATA SECRET OUT)
  string(RANDOM LENGTH 16 _RND)
  set(_in  "/tmp/_hmac_in_${_RND}")
  set(_bin "/tmp/_hmac_bin_${_RND}")

  file(WRITE "${_in}" "${DATA}")

  # 1) HMAC -> сырые 32 байта в файл
  execute_process(
    COMMAND openssl dgst -sha256 -hmac "${SECRET}" -binary
    INPUT_FILE "${_in}"
    OUTPUT_FILE "${_bin}"
    ERROR_QUIET
    RESULT_VARIABLE RC1
  )
  if(NOT RC1 EQUAL 0 OR NOT EXISTS "${_bin}")
    file(REMOVE "${_in}" "${_bin}")
    set(${OUT} "" PARENT_SCOPE)
    return()
  endif()

  # 2) сырые байты -> base64-текст (в CMake-строку, без \0)
  execute_process(
    COMMAND openssl base64 -A -in "${_bin}"
    OUTPUT_VARIABLE B64
    ERROR_QUIET
    RESULT_VARIABLE RC2
    INPUT_FILE /dev/null
  )
  file(REMOVE "${_in}" "${_bin}")

  if(NOT RC2 EQUAL 0 OR B64 STREQUAL "")
    set(${OUT} "" PARENT_SCOPE)
    return()
  endif()
  string(STRIP "${B64}" B64)
  b64_to_b64url("${B64}" U)
  set(${OUT} "${U}" PARENT_SCOPE)
endfunction()