// The sockets, through the C layer in io.c (epoll). A client is a slot number; Goose hears about the world as a flat array of
// events and answers with send, close and poll.
namespace io;

let ACCEPT = 1;             // a client connected: id is its slot, its handle from now on
let RECV = 2;               // bytes arrived for client id: res of them, at inbox[off..]. res == 0: it closed; res < 0: error
let SEND = 3;               // the response queued for client id is out (res > 0), or its connection failed (res < 0)
let POLL = 4;               // Postgres socket number id is ready: res is the poll mask

let POLLIN = 0x1;
let POLLOUT = 0x4;
let POLLERR = 0x8;
let POLLHUP = 0x10;

struct Event { kind: u32, id: u32, res: i32, off: u32 }      // as event_t in io.c

// Starts this thread's epoll set and its listeners (port2 < 0: only one). host is NUL-terminated. Returns 0, or -errno.
extern fn start(host: const u8&, port1: i32, port2: i32) -> i32 = "io_start";

// Waits for events. After a busy iteration (min_events > 1), if that iteration had fewer events than min_events, sleeps
// timeout_us first so that more of them pile up; then waits for the first. Stores up to max events at ev, and the bytes
// received in inbox (cap of them). Returns how many events.
extern fn wait(ev: Event&, max: i32, inbox: u8&, cap: i64, min_events: i32, timeout_us: i64) -> i32 = "io_wait";

// Sends n bytes to the client in slot. Returns 1 if they are all out already, 0 if the rest waits for the socket (with
// notify, a SEND event says when it is out), -1 if the connection failed.
extern fn send(slot: u32, buf: const u8&, n: i64, notify: bool) -> i32 = "io_send";

// Done with the client in slot: its socket is closed.
extern fn close(slot: u32) = "io_close";

// Postgres sockets belong to libpq. poll asks for POLL events when fd is ready for mask; ask again after each event.
extern fn poll(i: i32, fd: i32, mask: u32) = "io_poll";
extern fn poll_cancel(i: i32) = "io_poll_cancel";
