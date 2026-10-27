cmake_minimum_required(VERSION 3.25)

set(_d "${CMAKE_CURRENT_LIST_DIR}")
include("${_d}/http.cmake")
include("${_d}/json.cmake")
include("${_d}/b64.cmake")
include("${_d}/jwt.cmake")
include("${_d}/routes.cmake")

# ---------- config ----------
if(DEFINED ENV{JWT_SECRET})
  set(JWT_SECRET "$ENV{JWT_SECRET}")
else()
  set(JWT_SECRET "million-users-challenge")
endif()

if(DEFINED ENV{START_TIME})
  set(START_TIME "$ENV{START_TIME}")
else()
  set(START_TIME 0)
endif()

# ---------- читаем stdin НА TOP-LEVEL ----------
execute_process(
  COMMAND cat
  OUTPUT_VARIABLE RAW
  ERROR_QUIET
)

# ---------- роутинг ----------
function(handle_request RAW)
  parse_request("${RAW}")

  if(METHOD STREQUAL "")
    send_error(400 "Bad Request" "malformed JSON body")
    return()
  endif()

  if(METHOD STREQUAL "GET" AND PATH STREQUAL "/health")
    handle_health()
    return()
  endif()

  if(METHOD STREQUAL "GET" AND PATH STREQUAL "/feed")
    handle_feed()
    return()
  endif()

  if(METHOD STREQUAL "GET" AND PATH MATCHES "^/posts/([^/]+)$")
    set(ID "${CMAKE_MATCH_1}")
    if(NOT ID MATCHES "^[1-9][0-9]*$")
      send_error(400 "Bad Request" "invalid post id")
      return()
    endif()
    handle_get_post("${ID}")
    return()
  endif()

  if(METHOD STREQUAL "POST" AND PATH STREQUAL "/posts")
    require_auth("${AUTH}" "${JWT_SECRET}")
    if(NOT AUTH_PASSED)
      return()
    endif()
    handle_create_post("${AUTH_SUB}" "${AUTH_USERNAME}" "${BODY}")
    return()
  endif()

  if(METHOD STREQUAL "POST" AND PATH MATCHES "^/posts/([^/]+)/like$")
    require_auth("${AUTH}" "${JWT_SECRET}")
    if(NOT AUTH_PASSED)
      return()
    endif()
    set(ID "${CMAKE_MATCH_1}")
    if(NOT ID MATCHES "^[1-9][0-9]*$")
      send_error(400 "Bad Request" "invalid post id")
      return()
    endif()
    handle_like("${ID}" "${AUTH_SUB}")
    return()
  endif()

  send_error(404 "Not Found" "not found")
endfunction()

handle_request("${RAW}")