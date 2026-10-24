//! The event loop: one thread per worker, each with its own io_uring, SO_REUSEPORT listeners (both ports), connection
//! table and Postgres connections. Nothing on the request path is shared between workers.
//!
//! Client sockets are io_uring *direct descriptors*: accepted straight into the ring's fixed-file table (no process
//! file descriptor, no per-operation fd lookup), and the table index is the connection's id. An idle keep-alive
//! connection costs a 16-byte record here plus the kernel's socket and its armed multishot recv; receive buffers come
//! from a shared provided-buffer ring and belong to a connection only while one of its requests is incomplete.
const std = @import("std");
const linux = std.os.linux;
const IoUring = linux.IoUring;
const sys = @import("sys.zig");
const http = @import("http.zig");
const pg = @import("pg.zig");
const jwt = @import("jwt.zig");
const api = @import("api.zig");
const sql = @import("sql.zig");
const deflate = @import("deflate.zig");
const Buf = @import("buf.zig").Buf;
const gpa = @import("buf.zig").gpa;
const Scratch = @import("scratch.zig").Scratch;

const log = std.log.scoped(.server);

pub const Config = struct {
    host: [4]u8,
    ports: [2]u16,
    db: pg.Config,
    workers: u32,
    pg_read: u32, // connections per worker
    pg_write: u32,
    pg_depth: u32, // requests in flight per connection
    pg_pack: u32, // consecutive reads given to one connection before moving to the next (1 = spread evenly)
    slots: u32, // direct descriptors per ring
    coalesce_us: u32, // how long a busy loop waits for more completions (0 = never)
    coalesce_min: u32,
    stats: bool,
    test_fail_groups: bool, // test hook: poison every multi-member write group so the retry path runs
    test_fail_packs: bool, // test hook: make the first attempt of every read pack fail so the resend path runs
};

pub const Shared = struct {
    cfg: Config,
    jwt: jwt.Key,
    start_ns: u64,
    ready: u32 = 0, // workers that finished start-up
};

pub const Class = enum(u1) { read, write };

// ---------------------------------------------------------------------------------------------
// Requests in flight (waiting for Postgres)
// ---------------------------------------------------------------------------------------------

pub const Req = struct {
    next: ?*Req = null,
    slot: u32 = 0,
    gen: u32 = 0,
    kind: api.Kind = .health,
    keep_alive: bool = true,
    gzip: bool = false,
    tries: u8 = 0, // times a read has been sent to Postgres
    uid: i64 = 0,
    aux: i64 = 0, // post id (reads, likes) or body length (creates)
    blob: []u8 = &.{},
};

// ---------------------------------------------------------------------------------------------
// Connections
// ---------------------------------------------------------------------------------------------

const Flags = packed struct(u8) {
    open: bool = false,
    busy: bool = false, // a request is being answered; later bytes wait in `pend`
    close_after: bool = false,
    recv_live: bool = false,
    send_live: bool = false,
    closing: bool = false,
    eof: bool = false, // the peer closed its sending side; finish what is in flight, then close
    tracked: bool = false, // listed in Worker.partials
};

/// 16 bytes per connection. `ext` is only attached while there are buffered request bytes or a response in flight.
const Conn = struct {
    gen: u32 = 0,
    flags: Flags = .{},
    ext: ?*Ext = null,
};

comptime {
    std.debug.assert(@sizeOf(Conn) == 16);
}

const Ext = struct {
    next: ?*Ext = null,
    pend: [*]u8 = undefined,
    pend_len: u32 = 0,
    pend_cap: u32 = 0,
    out: [*]u8 = undefined, // start of the response (its head is right-aligned inside the buffer)
    out_base: [*]u8 = undefined, // the pool buffer holding it
    out_len: u32 = 0,
    out_off: u32 = 0,
    out_cap: u32 = 0,
    out_class: u8 = 0,
    since: u32 = 0, // seconds (worker clock) at which an incomplete request started waiting
};

const max_pend = http.max_body + http.max_head + 4096;

// user_data layout: tag (8) | gen (24) | index (32)
const Tag = enum(u8) { accept = 1, recv, send, pg_recv, pg_send, event, close, shut, timer };

inline fn ud(tag: Tag, gen: u32, idx: u32) u64 {
    return (@as(u64, @backingInt(tag)) << 56) | (@as(u64, gen & 0xffffff) << 32) | idx;
}

/// Response buffers by size class (512 B ... 128 KB); larger ones come straight from the allocator.
const Pool = struct {
    const Node = struct { next: ?*Node };
    const n_classes = 9;
    const keep = 512; // free buffers kept per class
    free: [n_classes]?*Node = @splat(null),
    count: [n_classes]u32 = @splat(0),

    fn classOf(n: usize) u8 {
        if (n > (512 << (n_classes - 1))) return 255;
        var c: u8 = 0;
        while ((@as(usize, 512) << @intCast(c)) < n) c += 1;
        return c;
    }

    fn alloc(self: *Pool, n: usize, class: *u8, cap: *u32) [*]u8 {
        const c = classOf(n);
        class.* = c;
        if (c == 255) {
            cap.* = @intCast(n);
            return (gpa.alloc(u8, n) catch @panic("out of memory")).ptr;
        }
        cap.* = @intCast(@as(usize, 512) << @intCast(c));
        if (self.free[c]) |node| {
            self.free[c] = node.next;
            self.count[c] -= 1;
            return @ptrCast(node);
        }
        return (gpa.alloc(u8, cap.*) catch @panic("out of memory")).ptr;
    }

    fn release(self: *Pool, p: [*]u8, class: u8, cap: u32) void {
        if (class == 255 or self.count[class] >= keep) return gpa.free(p[0..cap]);
        const node: *Node = @ptrCast(@alignCast(p));
        node.next = self.free[class];
        self.free[class] = node;
        self.count[class] += 1;
    }
};

// ---------------------------------------------------------------------------------------------
// Postgres connections
// ---------------------------------------------------------------------------------------------

const qcap = 128; // reply ring per connection (pg_depth is clamped below this)

const prepare_list = blk: {
    var out: [sql.prepared.len]pg.Prep = undefined;
    for (sql.prepared, 0..) |x, i| out[i] = .{ .id = x.id, .sql = x.sql, .params = x.params };
    break :blk out;
};

/// One write inside a group commit: its encoded statement (with its own trailing Sync, used if the group has to be
/// retried member by member).
const Member = struct { r: *Req, bytes: []u8 };
const max_group = 64;
const sync_len = 5;

const WRec = struct {
    n: u32,
    m: [max_group]Member,
};

/// Reads sent to one connection in one pass of the loop: their Bind/Execute pairs and a single Sync, so Postgres
/// answers them in one flush instead of one per query. Each member's reply is delivered as soon as its CommandComplete
/// arrives. A null member is the test hook's poison statement.
const max_pack = 128;
const RRec = struct {
    n: u32,
    done: u32, // members answered so far
    m: [max_pack]?*Req,
};

const PgConn = struct {
    fd: i32 = -1,
    class: Class = .read,
    alive: bool = false,
    sending: bool = false,
    recving: bool = false,
    dirty: bool = false,
    rs: pg.Stream = undefined,
    wa: Buf = .{}, // in flight (immutable while `sending`)
    ws: Buf = .{}, // staging
    wsent: usize = 0,
    q: [qcap]usize = undefined, // *Req, *RRec | 1, or *WRec | 2
    qh: u32 = 0,
    nq: u32 = 0, // entries in `q`
    depth: u32 = 0, // requests in flight (a read pack counts each member, a write group counts once)
    pack: ?*RRec = null, // reads staged in `ws` during this pass, not yet closed with a Sync
    ginflight: bool = false, // a group commit is in flight on this (write) connection
    staged: std.ArrayList(Member) = .empty, // writes waiting for the in-flight group to commit
};

const Queued = struct { r: *Req, bytes: []u8, since: u32 };

/// How long a request may wait for a Postgres connection (all busy, or all down and being re-established).
const max_wait_s = 3;

const Fifo = struct {
    items: []Queued = &.{},
    head: usize = 0,
    len: usize = 0,

    fn push(self: *Fifo, q: Queued) void {
        if (self.len == self.items.len) {
            const ncap = if (self.items.len == 0) 256 else self.items.len * 2;
            const n = gpa.alloc(Queued, ncap) catch @panic("out of memory");
            for (0..self.len) |i| n[i] = self.items[(self.head + i) % self.items.len];
            if (self.items.len != 0) gpa.free(self.items);
            self.items = n;
            self.head = 0;
        }
        self.items[(self.head + self.len) % self.items.len] = q;
        self.len += 1;
    }

    fn pop(self: *Fifo) ?Queued {
        if (self.len == 0) return null;
        const q = self.items[self.head];
        self.head = (self.head + 1) % self.items.len;
        self.len -= 1;
        return q;
    }
};

// ---------------------------------------------------------------------------------------------
// Worker
// ---------------------------------------------------------------------------------------------

const buf_size = 2048;
const n_bufs = 2048;
const bgid = 1;

pub const Worker = struct {
    shared: *Shared,
    id: usize,
    ring: IoUring = undefined,
    listeners: [2]i32 = .{ -1, -1 },
    conns: []Conn = &.{},
    now: i64 = 0,
    out: Buf = .{},
    enc: Buf = .{},
    deflater: *deflate.Deflater = undefined,
    scratch: Scratch = undefined,
    pool: Pool = .{},
    free_reqs: ?*Req = null,
    free_ext: ?*Ext = null,
    pgs: []PgConn = &.{},
    n_read: usize = 0,
    rr: [2]usize = .{ 0, 0 },
    pack_conn: [2]?*PgConn = .{ null, null },
    pack_left: [2]u32 = .{ 0, 0 },
    fifo: [2]Fifo = .{ .{}, .{} },
    dirty: std.ArrayList(u32) = .empty,
    synth: Buf = .{},
    efd: i32 = -1,
    efd_val: u64 = 0,
    pbuf_ring: [*]linux.io_uring_buf = undefined,
    pbuf_mem: [*]u8 = undefined,
    pbuf_tail: u16 = 0,
    accept_live: [2]bool = .{ false, false },
    partials: std.ArrayList(u32) = .empty,
    tick: linux.kernel_timespec = .{ .sec = 1, .nsec = 0 },
    coalesce_ns: u32 = 0,
    coalesce_min: u32 = 1,
    rc_busy: u32 = 0, // a reconnect thread is running (atomic)
    rc_fd: i32 = -1, // a connected, prepared socket waiting for the loop to adopt (atomic)
    stats: struct { reqs: u64 = 0, loops: u64 = 0, cqes: u64 = 0, accepts: u64 = 0, fifo_max: usize = 0, groups: u64 = 0, grouped: u64 = 0 } = .{},

    // ---- small helpers ----------------------------------------------------------------------

    /// Start rendering a response body into `out`, behind the room reserved for the head.
    pub fn beginBody(self: *Worker) void {
        self.out.len = 0;
        self.out.ensure(http.head_room + 1024);
        self.out.len = http.head_room;
    }

    pub fn uptimeSecs(self: *const Worker) u64 {
        return (sys.nowNs() - self.shared.start_ns) / 1_000_000_000;
    }

    fn sqe(self: *Worker) *linux.io_uring_sqe {
        while (true) {
            return self.ring.get_sqe() catch {
                _ = self.ring.submit() catch {};
                continue;
            };
        }
    }

    pub fn newReq(self: *Worker) *Req {
        const r = self.free_reqs orelse blk: {
            const n = 256; // refill: a slab of requests
            const slab = gpa.alloc(Req, n) catch @panic("out of memory");
            for (slab[1..]) |*x| {
                x.* = .{};
                x.next = self.free_reqs;
                self.free_reqs = x;
            }
            slab[0] = .{};
            break :blk &slab[0];
        };
        if (self.free_reqs == r) self.free_reqs = r.next;
        r.* = .{};
        return r;
    }

    pub fn freeReq(self: *Worker, r: *Req) void {
        if (r.blob.len != 0) gpa.free(r.blob);
        r.blob = &.{};
        r.next = self.free_reqs;
        self.free_reqs = r;
    }

    fn getExt(self: *Worker, c: *Conn) *Ext {
        if (c.ext) |e| return e;
        const e = self.free_ext orelse blk: {
            const n = 256;
            const slab = gpa.alloc(Ext, n) catch @panic("out of memory");
            for (slab[1..]) |*x| {
                x.* = .{};
                x.next = self.free_ext;
                self.free_ext = x;
            }
            slab[0] = .{};
            break :blk &slab[0];
        };
        if (self.free_ext == e) self.free_ext = e.next;
        e.* = .{};
        c.ext = e;
        return e;
    }

    /// Give the extension back once nothing is buffered or in flight.
    fn dropExt(self: *Worker, c: *Conn) void {
        const e = c.ext orelse return;
        if (e.pend_len != 0 or e.out_len != 0) return;
        if (e.pend_cap != 0) gpa.free(e.pend[0..e.pend_cap]);
        e.next = self.free_ext;
        self.free_ext = e;
        c.ext = null;
    }

    // ---- sending responses ------------------------------------------------------------------

    /// Turn the body in `out` into a response (compressing it when the client accepts gzip and it is worth it), and send
    /// it on the connection if that connection is still the same one.
    pub fn finish(self: *Worker, slot: u32, gen: u32, code: u16, keep_alive: bool, gzip_ok: bool) void {
        const c = &self.conns[slot];
        if (!c.flags.open or c.gen != gen) return;
        const body = self.out.ptr[http.head_room..self.out.len];
        const ext = self.getExt(c);
        var compressed = false;
        var buf: [*]u8 = undefined;
        var body_len: usize = body.len;
        if (gzip_ok and body.len >= 512 and code != 204) {
            // compress straight into the response buffer, behind the room for the head
            buf = self.pool.alloc(http.head_room + deflate.bound(body.len), &ext.out_class, &ext.out_cap);
            body_len = self.deflater.gzip(buf[http.head_room..][0..deflate.bound(body.len)], body);
            compressed = true;
        } else {
            buf = self.pool.alloc(http.head_room + body.len, &ext.out_class, &ext.out_cap);
            @memcpy(buf[http.head_room..][0..body.len], body);
        }
        const resp = http.finishResponse(buf[0 .. http.head_room + body_len], body_len, code, keep_alive, compressed);
        ext.out = resp.ptr;
        ext.out_len = @intCast(resp.len);
        ext.out_off = 0;
        // keep the allocation base for release: `resp` starts inside the buffer
        ext.out_base = buf;
        c.flags.close_after = !keep_alive;
        self.stats.reqs += 1;
        self.submitSend(slot, c, ext);
    }

    fn submitSend(self: *Worker, slot: u32, c: *Conn, ext: *Ext) void {
        const s = self.sqe();
        s.prep_send(@intCast(slot), (ext.out + ext.out_off)[0 .. ext.out_len - ext.out_off], linux.MSG.NOSIGNAL);
        s.flags |= linux.IOSQE_FIXED_FILE;
        s.user_data = ud(.send, c.gen, slot);
        c.flags.send_live = true;
    }

    fn onSend(self: *Worker, slot: u32, gen: u32, res: i32) void {
        const c = &self.conns[slot];
        if (c.gen & 0xffffff != gen) return;
        c.flags.send_live = false;
        const ext = c.ext orelse return;
        if (res < 0) {
            self.freeOut(ext);
            self.closeConn(slot);
            self.maybeFinishClose(slot);
            return;
        }
        ext.out_off += @intCast(res);
        if (ext.out_off < ext.out_len and c.flags.open) {
            self.submitSend(slot, c, ext);
            return;
        }
        self.freeOut(ext);
        if (!c.flags.open) {
            self.maybeFinishClose(slot);
            return;
        }
        c.flags.busy = false;
        if (c.flags.close_after) {
            self.closeConn(slot);
            return;
        }
        if (ext.pend_len != 0) self.parsePend(slot, c);
        if (c.flags.open) {
            if (c.flags.eof and !c.flags.busy) return self.closeConn(slot);
            self.dropExt(c);
        }
    }

    fn freeOut(self: *Worker, ext: *Ext) void {
        if (ext.out_len != 0) self.pool.release(ext.out_base, ext.out_class, ext.out_cap);
        ext.out_len = 0;
        ext.out_off = 0;
    }

    // ---- connection lifecycle ---------------------------------------------------------------

    fn onAccept(self: *Worker, which: usize, res: i32, flags: u32) void {
        if (flags & linux.IORING_CQE_F_MORE == 0) self.accept_live[which] = false;
        if (res < 0) {
            const err: linux.E = @fromBackingInt(@intCast(@as(u16, @intCast(-res))));
            if (err != .NFILE and err != .MFILE and err != .NOMEM) log.err("accept failed: {s}", .{@tagName(err)});
            return;
        }
        const slot: u32 = @intCast(res);
        if (slot >= self.conns.len) return; // cannot happen: the table has exactly `conns.len` entries
        const c = &self.conns[slot];
        if (c.ext != null) { // a previous connection in this slot left its extension behind (closed mid-send)
            const e = c.ext.?;
            if (e.out_len != 0) self.freeOut(e);
            e.pend_len = 0;
            self.dropExt(c);
        }
        c.* = .{ .gen = c.gen +% 1, .flags = .{ .open = true } };
        self.stats.accepts += 1;
        self.armRecv(slot, c);
    }

    fn armRecv(self: *Worker, slot: u32, c: *Conn) void {
        const s = self.sqe();
        s.prep_rw(.RECV, @intCast(slot), 0, 0, 0);
        s.ioprio |= linux.IORING_RECV_MULTISHOT;
        s.flags |= linux.IOSQE_FIXED_FILE | linux.IOSQE_BUFFER_SELECT;
        s.buf_index = bgid;
        s.user_data = ud(.recv, c.gen, slot);
        c.flags.recv_live = true;
    }

    fn closeConn(self: *Worker, slot: u32) void {
        const c = &self.conns[slot];
        if (!c.flags.open) return;
        c.flags.open = false;
        c.flags.closing = true;
        if (c.ext) |e| {
            if (e.pend_cap != 0) gpa.free(e.pend[0..e.pend_cap]);
            e.pend_len = 0;
            e.pend_cap = 0;
        }
        if (c.flags.recv_live or c.flags.send_live) {
            const s = self.sqe();
            s.prep_rw(.SHUTDOWN, @intCast(slot), 0, linux.SHUT.RDWR, 0);
            s.flags |= linux.IOSQE_FIXED_FILE;
            s.user_data = ud(.shut, 0, 0);
        }
        self.maybeFinishClose(slot);
    }

    fn maybeFinishClose(self: *Worker, slot: u32) void {
        const c = &self.conns[slot];
        if (!c.flags.closing or c.flags.recv_live or c.flags.send_live) return;
        if (c.ext) |e| {
            self.freeOut(e);
            e.pend_len = 0;
            self.dropExt(c);
        }
        c.flags.closing = false;
        c.flags.busy = false;
        const s = self.sqe();
        s.prep_close_direct(slot);
        s.user_data = ud(.close, 0, 0);
    }

    // ---- receiving --------------------------------------------------------------------------

    fn returnBuf(self: *Worker, bid: u16) void {
        const idx = self.pbuf_tail & (n_bufs - 1);
        self.pbuf_ring[idx].addr = @intFromPtr(self.pbuf_mem + @as(usize, bid) * buf_size);
        self.pbuf_ring[idx].len = buf_size;
        self.pbuf_ring[idx].bid = bid;
        self.pbuf_tail +%= 1;
    }

    fn publishBufs(self: *Worker) void {
        const br: *linux.io_uring_buf_ring = @ptrCast(self.pbuf_ring);
        @atomicStore(u16, &br.tail, self.pbuf_tail, .release);
    }

    fn onRecv(self: *Worker, slot: u32, gen: u32, res: i32, flags: u32) void {
        const has_buf = flags & linux.IORING_CQE_F_BUFFER != 0;
        const bid: u16 = @intCast(flags >> linux.IORING_CQE_BUFFER_SHIFT);
        const c = &self.conns[slot];
        const stale = c.gen & 0xffffff != gen;
        if (!stale and flags & linux.IORING_CQE_F_MORE == 0) c.flags.recv_live = false;
        defer if (has_buf) self.returnBuf(bid);
        if (stale) return;
        if (res > 0 and c.flags.open) {
            const data = (self.pbuf_mem + @as(usize, bid) * buf_size)[0..@intCast(res)];
            self.onData(slot, c, data);
        }
        if (res == 0 and c.flags.open and c.flags.busy) {
            c.flags.eof = true; // half-close: the response to the request in flight is still owed
        } else if (res <= 0 and c.flags.open) {
            const err: linux.E = @fromBackingInt(@intCast(@as(u16, @intCast(-res))));
            if (res == 0 or (err != .NOBUFS and err != .AGAIN and err != .INTR)) {
                self.closeConn(slot);
            } else if (!c.flags.recv_live) {
                self.armRecv(slot, c); // out of provided buffers: try again
            }
        } else if (c.flags.open and !c.flags.recv_live) {
            self.armRecv(slot, c);
        } else if (!c.flags.open) {
            self.maybeFinishClose(slot);
        }
    }

    /// New bytes from the socket.
    fn onData(self: *Worker, slot: u32, c: *Conn, data: []const u8) void {
        const pending = if (c.ext) |e| e.pend_len != 0 else false;
        if (!pending and !c.flags.busy) {
            const used = self.parseLoop(slot, c, data);
            if (used < data.len and c.flags.open) self.stash(slot, c, data[used..]);
        } else {
            self.stash(slot, c, data);
            if (!c.flags.busy and c.flags.open) self.parsePend(slot, c);
        }
    }

    fn stash(self: *Worker, slot: u32, c: *Conn, data: []const u8) void {
        const e = self.getExt(c);
        const need = e.pend_len + data.len;
        if (need > max_pend) {
            self.closeConn(slot);
            return;
        }
        if (need > e.pend_cap) {
            var ncap: usize = @max(512, e.pend_cap);
            while (ncap < need) ncap *= 2;
            const m = gpa.alloc(u8, ncap) catch {
                self.closeConn(slot);
                return;
            };
            if (e.pend_len != 0) @memcpy(m[0..e.pend_len], e.pend[0..e.pend_len]);
            if (e.pend_cap != 0) gpa.free(e.pend[0..e.pend_cap]);
            e.pend = m.ptr;
            e.pend_cap = @intCast(ncap);
        }
        @memcpy(e.pend[e.pend_len..][0..data.len], data);
        if (e.pend_len == 0) e.since = @intCast(self.now & 0x7fffffff);
        e.pend_len += @intCast(data.len);
        if (!c.flags.tracked) {
            c.flags.tracked = true;
            self.partials.append(gpa, slot) catch {};
        }
    }

    /// Close connections that have held an incomplete request for too long (slow or stalled clients), so they cannot
    /// pin memory indefinitely. Runs from the 1 s timer. Idle connections with nothing buffered are never listed.
    fn sweepPartials(self: *Worker) void {
        const limit_s = 30;
        const now: u32 = @intCast(self.now & 0x7fffffff);
        var i: usize = 0;
        while (i < self.partials.items.len) {
            const slot = self.partials.items[i];
            const c = &self.conns[slot];
            const pend_len = if (c.ext) |e| e.pend_len else 0;
            var drop = !c.flags.open or pend_len == 0;
            if (!drop and !c.flags.busy and now -% c.ext.?.since > limit_s) {
                self.closeConn(slot);
                drop = true;
            }
            if (drop) {
                c.flags.tracked = false;
                _ = self.partials.swapRemove(i);
            } else i += 1;
        }
    }

    fn parsePend(self: *Worker, slot: u32, c: *Conn) void {
        const e = c.ext.?;
        const data = e.pend[0..e.pend_len];
        const used = self.parseLoop(slot, c, data);
        if (!c.flags.open) return;
        if (used >= data.len) {
            gpa.free(e.pend[0..e.pend_cap]);
            e.pend_len = 0;
            e.pend_cap = 0;
        } else if (used > 0) {
            std.mem.copyForwards(u8, e.pend[0 .. data.len - used], e.pend[used..data.len]);
            e.pend_len = @intCast(data.len - used);
        }
    }

    /// Parse and dispatch as many complete requests as `data` holds (one at a time). Returns bytes consumed.
    fn parseLoop(self: *Worker, slot: u32, c: *Conn, data: []const u8) usize {
        var off: usize = 0;
        while (!c.flags.busy and off < data.len and c.flags.open) {
            self.scratch.reset();
            const rest = data[off..];
            const head = switch (http.parseHead(rest)) {
                .need_more => break,
                .bad => |code| {
                    self.badRequest(slot, c, code);
                    return data.len;
                },
                .ok => |h| h,
            };
            const body = switch (http.takeBody(&head, rest[head.head_len..], &self.scratch)) {
                .need_more => break,
                .bad => |code| {
                    self.badRequest(slot, c, code);
                    return data.len;
                },
                .ok => |b| b,
            };
            off += head.head_len + body.consumed;
            c.flags.busy = true;
            api.handle(self, slot, c.gen, head, body.data);
        }
        return off;
    }

    fn badRequest(self: *Worker, slot: u32, c: *Conn, code: u16) void {
        c.flags.busy = true;
        self.beginBody();
        self.out.putSlice("{\"error\":\"bad request\"}");
        self.finish(slot, c.gen, code, false, false);
    }

    // ---- Postgres pool ----------------------------------------------------------------------

    fn classRange(self: *const Worker, class: Class) struct { lo: usize, hi: usize } {
        return switch (class) {
            .read => .{ .lo = 0, .hi = self.n_read },
            .write => .{ .lo = self.n_read, .hi = self.pgs.len },
        };
    }

    fn anyAlive(self: *const Worker, class: Class) bool {
        const range = self.classRange(class);
        for (self.pgs[range.lo..range.hi]) |*p| if (p.alive) return true;
        return false;
    }

    /// A live connection of the class with room. A new connection is the least loaded one (the start rotates so ties
    /// spread out), and it then takes `pg_pack` consecutive requests: they leave in one `send`, wake the backend once and
    /// come back in one `recv`. Spreading requests one per connection costs a system call and a wake-up each.
    fn pickConn(self: *Worker, class: Class) ?*PgConn {
        const ci = @backingInt(class);
        if (self.pack_conn[ci]) |p| {
            if (self.pack_left[ci] > 0 and p.alive and p.depth < self.shared.cfg.pg_depth) {
                self.pack_left[ci] -= 1;
                return p;
            }
        }
        const range = self.classRange(class);
        const cap = self.shared.cfg.pg_depth;
        const n = range.hi - range.lo;
        if (n == 0) return null;
        const start = self.rr[ci];
        self.rr[ci] = (start + 1) % n;
        var best: ?*PgConn = null;
        var i: usize = 0;
        while (i < n) : (i += 1) {
            const p = &self.pgs[range.lo + (start + i) % n];
            if (!p.alive or p.depth >= cap) continue;
            if (best == null or p.depth < best.?.depth) best = p;
        }
        self.pack_conn[ci] = best;
        self.pack_left[ci] = self.shared.cfg.pg_pack -| 1;
        return best;
    }

    fn markDirty(self: *Worker, p: *PgConn) void {
        if (p.dirty) return;
        p.dirty = true;
        self.dirty.append(gpa, @intCast(p - self.pgs.ptr)) catch @panic("out of memory");
    }

    /// Queue an encoded request (Bind/Execute/Sync) for `r`; the reply is delivered to `api.onPg`. When every connection
    /// is busy, or down and being re-established, the request waits (at most `max_wait_s`); a health probe never waits.
    pub fn pgSubmit(self: *Worker, class: Class, r: *Req, bytes: []const u8) void {
        if (r.kind != .create and r.kind != .like) r.tries +|= 1;
        if (self.pickConn(class)) |p| {
            if (class == .read) return self.readStage(p, r, bytes);
            return self.pgPush(p, @intFromPtr(r), bytes);
        }
        if (r.kind == .health and !self.anyAlive(class)) return api.onPgFailure(self, r);
        const copy = gpa.dupe(u8, bytes) catch return api.onPgFailure(self, r);
        const f = &self.fifo[@backingInt(class)];
        f.push(.{ .r = r, .bytes = copy, .since = @intCast(self.now & 0x7fffffff) });
        if (f.len > self.stats.fifo_max) self.stats.fifo_max = f.len;
    }

    /// Fail the requests that have waited too long for a connection (runs from the 1 s timer).
    fn expireFifo(self: *Worker) void {
        const now: u32 = @intCast(self.now & 0x7fffffff);
        for (&self.fifo) |*f| {
            while (f.len != 0 and now -% f.items[f.head].since > max_wait_s) {
                const q = f.pop().?;
                gpa.free(q.bytes);
                api.onPgFailure(self, q.r);
            }
        }
    }

    fn pgPush(self: *Worker, p: *PgConn, token: usize, bytes: []const u8) void {
        p.ws.putSlice(bytes);
        p.q[(p.qh + p.nq) % qcap] = token;
        p.nq += 1;
        p.depth += 1;
        self.markDirty(p);
    }

    /// Adds a read (Bind/Execute/Sync) to the connection's open pack, without its Sync.
    fn readStage(self: *Worker, p: *PgConn, r: *Req, bytes: []const u8) void {
        std.debug.assert(std.mem.endsWith(u8, bytes, "S\x00\x00\x00\x04"));
        const rec = p.pack orelse blk: {
            const x = gpa.create(RRec) catch return self.pgPush(p, @intFromPtr(r), bytes); // a pack of its own
            x.n = 0; // (create does not apply field defaults)
            x.done = 0;
            if (self.shared.cfg.test_fail_packs and r.tries <= 1) {
                // Bind to a statement that does not exist: Postgres reports it and skips the rest of the pack.
                p.ws.putSlice("B\x00\x00\x00\x11\x00s9999\x00\x00\x00\x00\x00\x00\x00");
                p.ws.putSlice("E\x00\x00\x00\x09\x00\x00\x00\x00\x00");
                x.m[0] = null;
                x.n = 1;
            }
            p.pack = x;
            break :blk x;
        };
        p.ws.putSlice(bytes[0 .. bytes.len - sync_len]);
        rec.m[rec.n] = r;
        rec.n += 1;
        p.depth += 1;
        if (rec.n == max_pack) self.closePack(p);
        self.markDirty(p);
    }

    fn closePack(self: *Worker, p: *PgConn) void {
        _ = self;
        const rec = p.pack orelse return;
        p.ws.putSlice("S\x00\x00\x00\x04");
        p.q[(p.qh + p.nq) % qcap] = @intFromPtr(rec) | 1;
        p.nq += 1;
        p.pack = null;
    }

    /// Sends a read again (it has no side effects) on another connection, a few times at most.
    fn resendRead(self: *Worker, r: *Req) void {
        if ((r.kind == .feed or r.kind == .post or r.kind == .health) and r.tries < 6) {
            self.enc.reset();
            var e = pg.Enc.init(&self.enc);
            switch (r.kind) {
                .feed => e.bind(sql.id(.feed)),
                .health => e.bind(sql.id(.health)),
                else => {
                    e.bind(sql.id(.post));
                    e.pInt8(r.aux);
                },
            }
            e.exec();
            e.sync();
            return self.pgSubmit(.read, r, self.enc.slice());
        }
        api.onPgFailure(self, r);
    }

    /// Delivers the replies of the read pack at the head of the queue as far as they have arrived. Returns true once its
    /// ReadyForQuery has been consumed.
    fn drainPack(self: *Worker, p: *PgConn, rec: *RRec) bool {
        while (p.rs.nextMsg()) |kind| {
            switch (kind) {
                'C', 'E' => {
                    // this member's reply is complete (an error makes Postgres skip the members after it)
                    const member = if (rec.done < rec.n) rec.m[rec.done] else null;
                    rec.done += 1;
                    if (member) |r| {
                        p.depth -= 1;
                        api.onPg(self, r, p.rs.pending());
                    }
                    p.rs.done();
                    if (kind == 'E') {
                        // skip to the ReadyForQuery, then send the skipped members again
                        while (rec.done < rec.n) : (rec.done += 1) {
                            if (rec.m[rec.done]) |r| {
                                p.depth -= 1;
                                self.resendRead(r);
                            }
                        }
                    }
                },
                'Z' => {
                    p.rs.done();
                    while (rec.done < rec.n) : (rec.done += 1) { // should not happen: members without a reply
                        if (rec.m[rec.done]) |r| {
                            p.depth -= 1;
                            self.resendRead(r);
                        }
                    }
                    gpa.destroy(rec);
                    return true;
                },
                else => {}, // BindComplete, DataRow, notices: part of the current member's reply
            }
        }
        return false;
    }

    /// Send what is staged on every connection touched during this pass of the loop.
    fn flushPg(self: *Worker) void {
        while (self.dirty.pop()) |idx| {
            const p = &self.pgs[idx];
            p.dirty = false;
            if (!p.alive) continue;
            self.closePack(p);
            if (p.class == .write and !p.ginflight and p.staged.items.len != 0) self.sendGroup(p);
            self.pgFlush(p);
        }
    }

    fn pgFlush(self: *Worker, p: *PgConn) void {
        if (p.ws.len == 0 or p.sending) return;
        const t = p.wa;
        p.wa = p.ws;
        p.ws = t;
        p.ws.reset();
        p.wsent = 0;
        p.sending = true;
        const idx: u32 = @intCast(p - self.pgs.ptr);
        const s = self.sqe();
        s.prep_send(p.fd, p.wa.slice(), linux.MSG.NOSIGNAL);
        s.user_data = ud(.pg_send, 0, idx);
    }

    fn onPgSend(self: *Worker, idx: u32, res: i32) void {
        const p = &self.pgs[idx];
        p.sending = false;
        if (res <= 0) return self.pgFail(p);
        p.wsent += @intCast(res);
        if (p.wsent < p.wa.len) {
            p.sending = true;
            const s = self.sqe();
            s.prep_send(p.fd, p.wa.slice()[p.wsent..], linux.MSG.NOSIGNAL);
            s.user_data = ud(.pg_send, 0, idx);
            return;
        }
        p.wa.reset();
        p.wsent = 0;
        if (p.ws.len != 0) self.pgFlush(p);
    }

    fn pgArmRecv(self: *Worker, p: *PgConn) void {
        if (p.recving or !p.alive) return;
        const space = p.rs.space(16 * 1024);
        const s = self.sqe();
        s.prep_recv(p.fd, space, 0);
        s.user_data = ud(.pg_recv, 0, @intCast(p - self.pgs.ptr));
        p.recving = true;
    }

    fn onPgRecv(self: *Worker, idx: u32, res: i32) void {
        const p = &self.pgs[idx];
        p.recving = false;
        if (res <= 0) return self.pgFail(p);
        p.rs.received(@intCast(res));
        while (p.nq != 0) {
            const token = p.q[p.qh];
            if (token & 3 == 1) {
                if (!self.drainPack(p, @ptrFromInt(token & ~@as(usize, 3)))) break;
                p.qh = (p.qh + 1) % qcap;
                p.nq -= 1;
                continue;
            }
            const batch = p.rs.nextBatch() orelse break;
            p.qh = (p.qh + 1) % qcap;
            p.nq -= 1;
            p.depth -= 1;
            if (token & 3 == 0) {
                api.onPg(self, @ptrFromInt(token), batch);
            } else {
                self.onGroupReply(p, @ptrFromInt(token & ~@as(usize, 3)), batch);
            }
            p.rs.done();
        }
        if (p.nq == 0) while (p.rs.nextBatch()) |_| {
            log.err("unexpected postgres reply", .{});
            p.rs.done();
        };
        self.pgDrain(p.class);
        self.pgArmRecv(p);
    }

    /// Move queued requests onto connections that have room.
    fn pgDrain(self: *Worker, class: Class) void {
        const f = &self.fifo[@backingInt(class)];
        while (f.len != 0) {
            const p = self.pickConn(class) orelse return;
            const q = f.pop().?;
            if (class == .read) self.readStage(p, q.r, q.bytes) else self.pgPush(p, @intFromPtr(q.r), q.bytes);
            gpa.free(q.bytes);
        }
    }

    fn pgFail(self: *Worker, p: *PgConn) void {
        if (!p.alive) return;
        log.err("postgres connection {d} lost", .{@as(usize, @intCast(p - self.pgs.ptr))});
        p.alive = false;
        _ = linux.close(p.fd);
        p.fd = -1;
        // Fail what was in flight on this connection; nothing is replayed (a write may have committed).
        var i: u32 = 0;
        while (i < p.nq) : (i += 1) self.failToken(p.q[(p.qh + i) % qcap]);
        if (p.pack) |rec| { // staged but never sent
            for (rec.m[0..rec.n]) |m| if (m) |r| self.resendRead(r);
            gpa.destroy(rec);
            p.pack = null;
        }
        p.depth = 0;
        p.nq = 0;
        p.qh = 0;
        p.ginflight = false;
        // Writes that were only staged have not been sent: another write connection can take them.
        const staged = p.staged.toOwnedSlice(gpa) catch &.{};
        for (staged) |mem| {
            if (self.anyAlive(.write)) self.writeSubmit(mem.r, mem.bytes) else api.onPgFailure(self, mem.r);
            gpa.free(mem.bytes);
        }
        if (staged.len != 0) gpa.free(staged);
        p.wa.reset();
        p.ws.reset();
        p.rs.start = 0;
        p.rs.scan = 0;
        p.rs.end = 0;
    }

    fn failToken(self: *Worker, token: usize) void {
        switch (token & 3) {
            0 => {
                const r: *Req = @ptrFromInt(token);
                // A read has no side effects, so it is sent again on another connection; a write may have committed.
                if (r.kind == .feed or r.kind == .post) return self.resendRead(r);
                api.onPgFailure(self, r);
            },
            1 => {
                const rec: *RRec = @ptrFromInt(token & ~@as(usize, 3));
                for (rec.m[rec.done..rec.n]) |m| if (m) |r| self.resendRead(r);
                gpa.destroy(rec);
            },
            else => {
                const rec: *WRec = @ptrFromInt(token & ~@as(usize, 3));
                for (rec.m[0..rec.n]) |mem| {
                    api.onPgFailure(self, mem.r);
                    gpa.free(mem.bytes);
                }
                gpa.destroy(rec);
            },
        }
    }

    /// Dead Postgres connections are replaced by a helper thread, so a slow or stalled server cannot freeze the loop;
    /// the loop adopts the finished socket from `onEvent`.
    fn pgReconnect(self: *Worker) void {
        var dead = false;
        for (self.pgs) |*p| dead = dead or !p.alive;
        if (!dead) return;
        if (@cmpxchgStrong(u32, &self.rc_busy, 0, 1, .acq_rel, .monotonic) != null) return;
        const t = std.Thread.spawn(.{ .stack_size = 256 * 1024 }, Worker.reconnectMain, .{self}) catch {
            @atomicStore(u32, &self.rc_busy, 0, .release);
            return;
        };
        t.detach();
    }

    fn reconnectMain(self: *Worker) void {
        if (pg.connect(self.shared.cfg.db)) |fd| {
            if (pg.prepareAll(fd, &prepare_list)) {
                pg.clearTimeouts(fd);
                setNonblock(fd);
                @atomicStore(i32, &self.rc_fd, fd, .release);
                var one: u64 = 1;
                _ = linux.write(self.efd, std.mem.asBytes(&one), 8);
                return;
            } else |_| _ = linux.close(fd);
        } else |_| {}
        @atomicStore(u32, &self.rc_busy, 0, .release);
    }

    fn adoptReconnected(self: *Worker, fd: i32) void {
        for (self.pgs) |*p| {
            if (p.alive) continue;
            p.fd = fd;
            p.alive = true;
            log.info("postgres connection re-established", .{});
            self.pgArmRecv(p);
            self.pgDrain(p.class);
            return;
        }
        _ = linux.close(fd);
    }

    fn onEvent(self: *Worker) void {
        const rfd = @atomicLoad(i32, &self.rc_fd, .acquire);
        if (rfd >= 0) {
            @atomicStore(i32, &self.rc_fd, -1, .release);
            self.adoptReconnected(rfd);
            @atomicStore(u32, &self.rc_busy, 0, .release);
            self.pgReconnect(); // next one, if any is still down
        }
        self.armEvent();
    }

    fn armEvent(self: *Worker) void {
        const s = self.sqe();
        s.prep_read(self.efd, std.mem.asBytes(&self.efd_val), 0);
        s.user_data = ud(.event, 0, 0);
    }

    // ---- writes: group commit ---------------------------------------------------------------

    /// Queue a write (Bind/Execute/Sync). Writes that arrive while a transaction is committing are sent together as one
    /// transaction; every response is sent after its commit.
    pub fn writeSubmit(self: *Worker, r: *Req, bytes: []const u8) void {
        const range = self.classRange(.write);
        var best: ?*PgConn = null;
        var best_load: usize = std.math.maxInt(usize);
        for (self.pgs[range.lo..range.hi]) |*p| {
            if (!p.alive) continue;
            const load = p.depth + p.staged.items.len;
            if (load < best_load) {
                best = p;
                best_load = load;
            }
        }
        const copy = gpa.dupe(u8, bytes) catch return api.onPgFailure(self, r);
        const p = best orelse {
            // No write connection is up (being re-established): nothing has been sent yet, so the write can wait.
            self.fifo[@backingInt(Class.write)].push(.{ .r = r, .bytes = copy, .since = @intCast(self.now & 0x7fffffff) });
            return;
        };
        p.staged.append(gpa, .{ .r = r, .bytes = copy }) catch {
            gpa.free(copy);
            return api.onPgFailure(self, r);
        };
        self.markDirty(p);
    }

    fn sendGroup(self: *Worker, p: *PgConn) void {
        const n = @min(p.staged.items.len, max_group);
        if (n == 0) return;
        const rec = gpa.create(WRec) catch return;
        rec.n = @intCast(n);
        self.enc.reset();
        for (0..n) |i| {
            rec.m[i] = p.staged.items[i];
            self.enc.putSlice(rec.m[i].bytes[0 .. rec.m[i].bytes.len - sync_len]); // members' statements, no Sync
        }
        if (self.shared.cfg.test_fail_groups and n >= 2) {
            // Bind to a statement that does not exist: the server answers with an error and rolls the group back.
            self.enc.putSlice("B\x00\x00\x00\x11\x00s9999\x00\x00\x00\x00\x00\x00\x00");
            self.enc.putSlice("E\x00\x00\x00\x09\x00\x00\x00\x00\x00");
        }
        self.enc.putSlice("S\x00\x00\x00\x04");
        const rest = p.staged.items.len - n;
        std.mem.copyForwards(Member, p.staged.items[0..rest], p.staged.items[n..]);
        p.staged.shrinkRetainingCapacity(rest);
        p.ginflight = true;
        self.stats.groups += 1;
        self.stats.grouped += n;
        p.ws.putSlice(self.enc.slice());
        p.q[(p.qh + p.nq) % qcap] = @intFromPtr(rec) | 2;
        p.nq += 1;
        p.depth += 1;
    }

    fn onGroupReply(self: *Worker, p: *PgConn, rec: *WRec, batch: []const u8) void {
        p.ginflight = false;
        var errored = false;
        var scan = pg.Reader{ .data = batch };
        while (scan.next()) |m| {
            if (m.kind == 'E') errored = true;
        }
        if (errored) {
            // Rolled back as a whole (nothing committed): run each member on its own.
            for (rec.m[0..rec.n]) |mem| {
                self.pgSubmit(.write, mem.r, mem.bytes);
                gpa.free(mem.bytes);
            }
        } else {
            // Every member is one statement, so its slice of the reply ends at its CommandComplete.
            var rd = pg.Reader{ .data = batch };
            var start: usize = 0;
            var idx: usize = 0;
            const syn = &self.synth;
            while (idx < rec.n) {
                const m = rd.next() orelse break;
                if (m.kind != 'C') continue;
                syn.reset();
                syn.putSlice(batch[start..rd.pos]);
                syn.putSlice("Z\x00\x00\x00\x05I");
                start = rd.pos;
                const mem = rec.m[idx];
                idx += 1;
                api.onPg(self, mem.r, syn.slice());
                gpa.free(mem.bytes);
            }
            while (idx < rec.n) : (idx += 1) { // reply shorter than expected: should not happen
                api.onPgFailure(self, rec.m[idx].r);
                gpa.free(rec.m[idx].bytes);
            }
        }
        gpa.destroy(rec);
        if (p.staged.items.len != 0) self.markDirty(p);
    }

    // ---- main loop --------------------------------------------------------------------------

    fn armAccept(self: *Worker, which: usize) void {
        const s = self.sqe();
        // (SOCK_CLOEXEC is invalid for direct descriptors: they are not process file descriptors)
        s.prep_multishot_accept_direct(self.listeners[which], null, null, linux.SOCK.NONBLOCK);
        s.user_data = ud(.accept, 0, @intCast(which));
        self.accept_live[which] = true;
    }

    fn armTimer(self: *Worker) void {
        const s = self.sqe();
        s.prep_timeout(&self.tick, 0, 0);
        s.user_data = ud(.timer, 0, 0);
    }

    fn dispatch(self: *Worker, cqe: linux.io_uring_cqe) void {
        const tag: Tag = @fromBackingInt(@intCast(@as(u8, @intCast(cqe.user_data >> 56))));
        const gen: u32 = @intCast((cqe.user_data >> 32) & 0xffffff);
        const idx: u32 = @truncate(cqe.user_data);
        switch (tag) {
            .recv => self.onRecv(idx, gen, cqe.res, cqe.flags),
            .send => self.onSend(idx, gen, cqe.res),
            .accept => self.onAccept(idx, cqe.res, cqe.flags),
            .pg_recv => self.onPgRecv(idx, cqe.res),
            .pg_send => self.onPgSend(idx, cqe.res),
            .event => self.onEvent(),
            .timer => {
                self.now = sys.nowUnix();
                for (0..2) |i| if (!self.accept_live[i]) self.armAccept(i);
                self.pgReconnect();
                self.expireFifo();
                self.sweepPartials();
                if (self.shared.cfg.stats) self.printStats();
                self.armTimer();
            },
            .close, .shut => {},
        }
    }

    fn printStats(self: *Worker) void {
        const st = &self.stats;
        std.debug.print("[w{d}] reqs={d} accepts={d} loops={d} cqes={d} groups={d} grouped={d} fifo_max={d}\n", .{ self.id, st.reqs, st.accepts, st.loops, st.cqes, st.groups, st.grouped, st.fifo_max });
    }

    /// Submit what is queued and wait for completions. A loop that was busy waits a little for more of them, so one
    /// `io_uring_enter` serves a batch instead of a single event; an idle loop wakes on the first.
    fn waitCqes(self: *Worker, busy: bool) void {
        const to_submit = self.ring.flush_sq();
        var flags: u32 = linux.IORING_ENTER_GETEVENTS;
        var min: u32 = 1;
        var ts: linux.kernel_timespec = undefined;
        var arg: linux.io_uring_getevents_arg = undefined;
        var argp: usize = 0;
        var argsz: usize = 0;
        if (busy and self.coalesce_ns != 0) {
            ts = .{ .sec = 0, .nsec = self.coalesce_ns };
            arg = .{ .sigmask = 0, .sigmask_sz = 0, .pad = 0, .ts = @intFromPtr(&ts) };
            flags |= linux.IORING_ENTER_EXT_ARG;
            argp = @intFromPtr(&arg);
            argsz = @sizeOf(linux.io_uring_getevents_arg);
            min = self.coalesce_min;
        }
        const rc = linux.syscall6(.io_uring_enter, @as(usize, @intCast(self.ring.fd)), to_submit, min, flags, argp, argsz);
        switch (sys.errnoOf(rc)) {
            .SUCCESS, .TIME, .INTR, .AGAIN, .BUSY => {},
            else => |e| {
                log.err("io_uring_enter: {s}", .{@tagName(e)});
                std.process.exit(1);
            },
        }
    }

    fn run(self: *Worker) void {
        var cqes: [256]linux.io_uring_cqe = undefined;
        var seen: u32 = 0;
        while (true) {
            self.waitCqes(seen >= 4);
            seen = 0;
            self.now = sys.nowUnix();
            self.stats.loops += 1;
            while (true) {
                const n = self.ring.copy_cqes(&cqes, 0) catch 0;
                if (n == 0) break;
                seen += n;
                self.stats.cqes += n;
                for (cqes[0..n]) |cqe| self.dispatch(cqe);
                self.publishBufs();
            }
            self.flushPg();
        }
    }
};

// ---------------------------------------------------------------------------------------------
// Start-up
// ---------------------------------------------------------------------------------------------

fn setNonblock(fd: i32) void {
    const fl = linux.fcntl(fd, linux.F.GETFL, 0);
    _ = linux.fcntl(fd, linux.F.SETFL, fl | @as(usize, 1 << @bitOffsetOf(linux.O, "NONBLOCK")));
}

fn initRing() !IoUring {
    const flag_sets = [_]u32{
        linux.IORING_SETUP_SINGLE_ISSUER | linux.IORING_SETUP_DEFER_TASKRUN | linux.IORING_SETUP_CQSIZE,
        linux.IORING_SETUP_COOP_TASKRUN | linux.IORING_SETUP_CQSIZE,
        linux.IORING_SETUP_CQSIZE,
    };
    // Resource failures are retried with smaller completion queues and then after a short wait: a server that does not
    // come up is not restarted, so a momentary shortage of kernel memory must not end it.
    const cq_sizes = [_]u32{ 65536, 16384, 4096 };
    var last: anyerror = error.ArgumentsInvalid;
    var attempt: u32 = 0;
    while (attempt < 50) : (attempt += 1) {
        for (cq_sizes) |cq| {
            for (flag_sets) |fl| {
                var p = std.mem.zeroInit(linux.io_uring_params, .{ .flags = fl });
                p.cq_entries = cq;
                if (IoUring.init_params(4096, &p)) |r| return r else |e| last = e;
            }
        }
        if (last != error.SystemResources) break;
        log.warn("io_uring_setup: {s}; retrying", .{@errorName(last)});
        sys.sleepMs(200);
    }
    return last;
}

fn setupPbufRing(w: *Worker) !void {
    const ring_bytes = n_bufs * @sizeOf(linux.io_uring_buf);
    const rmem = try sys.mapAnon(ring_bytes);
    var reg = std.mem.zeroInit(linux.io_uring_buf_reg, .{ .ring_addr = @intFromPtr(rmem.ptr), .ring_entries = n_bufs, .bgid = bgid });
    const rc = linux.io_uring_register(w.ring.fd, .REGISTER_PBUF_RING, &reg, 1);
    if (sys.errnoOf(rc) != .SUCCESS) {
        log.err("provided buffer rings are unavailable ({s}): this server needs Linux 6.0 or newer", .{@tagName(sys.errnoOf(rc))});
        return error.NoBufferRings;
    }
    const bmem = try sys.mapAnon(n_bufs * buf_size);
    w.pbuf_mem = bmem.ptr;
    w.pbuf_ring = @ptrCast(@alignCast(rmem.ptr));
    w.pbuf_tail = 0;
    for (0..n_bufs) |i| w.returnBuf(@intCast(i));
    w.publishBufs();
}

fn makeListener(cfg: *const Config, port: u16) !i32 {
    const s = linux.socket(linux.AF.INET, linux.SOCK.STREAM | linux.SOCK.NONBLOCK | linux.SOCK.CLOEXEC, 0);
    if (sys.errnoOf(s) != .SUCCESS) return error.SocketFailed;
    const fd: i32 = @intCast(s);
    const one: c_int = 1;
    // Accepted sockets inherit TCP_NODELAY: a response is one send, and its tail must not wait for an ACK.
    inline for (.{ .{ linux.SOL.SOCKET, linux.SO.REUSEADDR }, .{ linux.SOL.SOCKET, linux.SO.REUSEPORT }, .{ linux.IPPROTO.TCP, linux.TCP.NODELAY } }) |o| {
        _ = linux.setsockopt(fd, o[0], o[1], @ptrCast(&one), @sizeOf(c_int));
    }
    var sa: linux.sockaddr.in = .{ .port = std.mem.nativeToBig(u16, port), .addr = @bitCast(cfg.host) };
    if (sys.errnoOf(linux.bind(fd, @ptrCast(&sa), @sizeOf(linux.sockaddr.in))) != .SUCCESS) return error.BindFailed;
    if (sys.errnoOf(linux.listen(fd, 65535)) != .SUCCESS) return error.ListenFailed;
    return fd;
}

pub fn workerMain(shared: *Shared, id: usize) void {
    workerInit(shared, id) catch |e| {
        log.err("worker {d} failed to start: {s}", .{ id, @errorName(e) });
        std.process.exit(1);
    };
}

fn workerInit(shared: *Shared, id: usize) !void {
    const cfg = &shared.cfg;
    const w = try gpa.create(Worker);
    w.* = .{ .shared = shared, .id = id };
    w.scratch = try Scratch.init(64 * 1024);
    w.out = try Buf.init(64 * 1024);
    w.enc = try Buf.init(16 * 1024);
    w.synth = try Buf.init(64 * 1024);
    w.deflater = try gpa.create(deflate.Deflater);
    w.deflater.* = .{};
    w.coalesce_ns = cfg.coalesce_us * 1000;
    w.coalesce_min = cfg.coalesce_min;
    w.ring = try initRing();
    try setupPbufRing(w);
    w.ring.register_files_sparse(cfg.slots) catch |e| {
        log.err("cannot register {d} direct descriptors ({s}): LimitNOFILE too low, or kernel older than 5.19", .{ cfg.slots, @errorName(e) });
        return e;
    };
    w.now = sys.nowUnix();

    // connection table, indexed by direct descriptor (lazily committed by the kernel)
    const cmem = try sys.mapAnon(@as(usize, cfg.slots) * @sizeOf(Conn));
    w.conns = @as([*]Conn, @ptrCast(@alignCast(cmem.ptr)))[0..cfg.slots];

    // Postgres connections start out dead and are brought up by the reconnect thread, one after another, so a database
    // that is still starting (or briefly unreachable) never stops the server: until a connection is up, requests that
    // need one get an error and /health reports "degraded".
    const total = cfg.pg_read + cfg.pg_write;
    w.pgs = try gpa.alloc(PgConn, total);
    w.n_read = cfg.pg_read;
    for (w.pgs, 0..) |*p, i| {
        p.* = .{ .class = if (i < cfg.pg_read) .read else .write };
        p.rs = try pg.Stream.init(64 * 1024);
        p.wa = try Buf.init(8192);
        p.ws = try Buf.init(8192);
    }

    // mailbox for the reconnect thread
    const efd = linux.eventfd(0, linux.EFD.CLOEXEC);
    if (sys.errnoOf(efd) != .SUCCESS) return error.EventfdFailed;
    w.efd = @intCast(efd);

    for (0..2) |i| {
        w.listeners[i] = try makeListener(cfg, cfg.ports[i]);
        w.armAccept(i);
    }
    w.armEvent();
    w.armTimer();
    w.pgReconnect();
    _ = @atomicRmw(u32, &shared.ready, .Add, 1, .release);
    w.run();
}
