include("${CMAKE_CURRENT_LIST_DIR}/http.cmake")

set(RAW "GET /health HTTP/1.1\r\nHost: x\r\nContent-Length: 0\r\n\r\n")
string(LENGTH "${RAW}" RL)
message(STATUS "RAW_LEN=${RL}")

parse_request("${RAW}")
message(STATUS "RESULT METHOD='${METHOD}' PATH='${PATH}' AUTH='${AUTH}' BODY='${BODY}'")