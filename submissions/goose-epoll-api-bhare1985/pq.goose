// libpq in pipeline mode. A connection or a result is the C pointer as a u64 that Goose never dereferences.
// connect() and the send_* calls are in shim.c (they need C arrays and strings); the rest are libpq's own functions.
namespace pq;
import ffi;

type Conn = u64;
type Res = u64;

let COMMAND_OK = 1;
let TUPLES_OK = 2;
let FATAL_ERROR = 7;
let PIPELINE_SYNC = 10;

// Connects, prepares the n statements (text holds them NUL-terminated at offs[i], with np[i] parameters), enters pipeline
// mode and goes non-blocking. 0 on failure.
extern fn connect_n(url: const u8&, n: i32, text: const u8&, offs: const i32&, np: const i32&) -> Conn = "shim_pg_connect_n";
extern fn lost(c: Conn) = "shim_pg_lost";               // logs why the connection died

// Sends statement stmt with n text parameters: value i is the NUL-terminated string at buf + offs[i]. 0 if it could not
// be queued.
extern fn send_n(c: Conn, stmt: i32, n: i32, buf: const u8&, offs: const i32&) -> i32 = "shim_pg_send_n";
extern fn send_sync(c: Conn) -> i32 = "PQsendPipelineSync";

extern fn finish(c: Conn) = "PQfinish";
extern fn socket(c: Conn) -> i32 = "PQsocket";
extern fn flush(c: Conn) -> i32 = "PQflush";                   // 0 sent, 1 more to send, -1 error
extern fn consume_input(c: Conn) -> i32 = "PQconsumeInput";    // 0 on error
extern fn is_busy(c: Conn) -> i32 = "PQisBusy";
extern fn next_result(c: Conn) -> Res = "PQgetResult";         // 0 ends a statement's results

extern fn status(r: Res) -> i32 = "PQresultStatus";
extern fn fields(r: Res) -> i32 = "PQnfields";
extern fn rows(r: Res) -> i32 = "PQntuples";
extern fn clear(r: Res) = "PQclear";
extern fn affected(r: Res) -> i64 = "shim_affected";       // rows the command changed
extern fn error_field(r: Res, code: i32) -> u64 = "PQresultErrorField";    // 'C' (67): the SQLSTATE
extern fn ftype(r: Res, col: i32) -> u32 = "PQftype";
extern fn fname(r: Res, col: i32) -> u64 = "PQfname";
extern fn cstr_len(p: u64) -> i64 = "shim_cstr_len";
extern fn is_null(r: Res, row: i32, col: i32) -> i32 = "PQgetisnull";

extern fn value(r: Res, row: i32, col: i32) -> u64 = "PQgetvalue";
extern fn length(r: Res, row: i32, col: i32) -> i32 = "PQgetlength";

// Appends the bytes of a column of the result (text format) to out.
fn append_text(r: Res, row: i32, col: i32, out: u8[>..]&) {
    ffi::bytes(value(r, row, col), length(r, row, col), out);
}

// Appends a column's name to out.
fn append_name(r: Res, col: i32, out: u8[>..]&) {
    let p = fname(r, col);
    ffi::bytes(p, cstr_len(p), out);
}

// A result with rows and exactly this many columns.
fn rows_ok(r: Res, cols: i32) -> bool { r != 0 && status(r) == TUPLES_OK && fields(r) == cols }
