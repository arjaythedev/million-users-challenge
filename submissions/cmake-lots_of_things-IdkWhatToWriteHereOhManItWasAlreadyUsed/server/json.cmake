# server/json.cmake
# Ручная сборка JSON (числа без кавычек, точный порядок ключей) + экранирование.
# Парсинг входящего body — через string(JSON).

# ---- экранирование строки для JSON ----
# CMake не понимает "\b"/"\f", поэтому control-символы заменяем на их
# JSON-escapes через string(ASCII), без литералов в исходнике.
function(json_escape IN OUT)
  string(REPLACE "\\" "\\\\" T "${IN}")
  string(REPLACE "\"" "\\\"" T "${T}")
  string(REPLACE "\n" "\\n"  T "${T}")
  string(REPLACE "\r" "\\r"  T "${T}")
  string(REPLACE "\t" "\\t"  T "${T}")
  set(${OUT} "${T}" PARENT_SCOPE)
endfunction()

# ---- post-объект ----
function(json_post ID BODY CREATED AUTHOR LIKES OUT)
  json_escape("${BODY}" B)
  json_escape("${AUTHOR}" A)
  set(J "{\"id\":${ID},\"body\":\"${B}\",\"created_at\":\"${CREATED}\",\"author\":\"${A}\",\"like_count\":${LIKES}}")
  set(${OUT} "${J}" PARENT_SCOPE)
endfunction()

# ---- парсинг входящего body ----
# Возвращает:
#   OUT_FIELD  — trimmed body
#   OUT_OK     — TRUE/FALSE
#   OUT_ERR    — "malformed" | "required" | "too_long"
function(extract_body_field RAW_BODY OUT_FIELD OUT_OK OUT_ERR)
  set(${OUT_FIELD} "" PARENT_SCOPE)
  set(${OUT_OK} FALSE PARENT_SCOPE)
  set(${OUT_ERR} "" PARENT_SCOPE)

  if(RAW_BODY STREQUAL "")
    set(${OUT_ERR} "malformed" PARENT_SCOPE)
    return()
  endif()

  # 1. валиден ли JSON вообще? TYPE без ключа парсит верхний уровень.
  string(JSON _top_type ERROR_VARIABLE E_TYPE TYPE "${RAW_BODY}")
  if(E_TYPE)
    set(${OUT_ERR} "malformed" PARENT_SCOPE)
    return()
  endif()
  if(NOT _top_type STREQUAL "OBJECT")
    set(${OUT_ERR} "malformed" PARENT_SCOPE)
    return()
  endif()

  # 2. есть ли ключ body?
  string(JSON V ERROR_VARIABLE E GET "${RAW_BODY}" "body")
  if(E)
    set(${OUT_ERR} "required" PARENT_SCOPE)
    return()
  endif()

  # 3. body — строка?
  string(JSON T ERROR_VARIABLE E2 TYPE "${RAW_BODY}" "body")
  if(E2 OR NOT T STREQUAL "STRING")
    set(${OUT_ERR} "required" PARENT_SCOPE)
    return()
  endif()

  string(STRIP "${V}" V)
  if(V STREQUAL "")
    set(${OUT_ERR} "required" PARENT_SCOPE)
    return()
  endif()

  string(LENGTH "${V}" L)
  if(L GREATER 500)
    set(${OUT_ERR} "too_long" PARENT_SCOPE)
    return()
  endif()

  set(${OUT_FIELD} "${V}" PARENT_SCOPE)
  set(${OUT_OK} TRUE PARENT_SCOPE)
endfunction()