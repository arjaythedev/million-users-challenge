package challenge;

import java.io.InputStream;
import java.io.OutputStream;
import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.net.InetAddress;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.net.URI;
import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;
import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;
import static java.lang.foreign.ValueLayout.JAVA_INT_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_LONG_UNALIGNED;
import static java.lang.foreign.ValueLayout.JAVA_SHORT_UNALIGNED;

public final class Pg {

    // =========================================================================
    // Official SPEC.md SQL Queries (Prepared Once at Connection Startup)
    // =========================================================================
    public static final String SQL_HEALTH = "SELECT 1";

    public static final String SQL_FEED =
        "SELECT p.id, p.body, p.created_at, u.username, " +
        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " +
        "FROM posts p JOIN users u ON u.id = p.user_id " +
        "ORDER BY p.created_at DESC, p.id DESC LIMIT 20";

    public static final String SQL_GET_POST =
        "SELECT p.id, p.body, p.created_at, u.username, " +
        "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " +
        "FROM posts p JOIN users u ON u.id = p.user_id " +
        "WHERE p.id = $1";

    public static final String SQL_INSERT_POST =
        "INSERT INTO posts (user_id, body) VALUES ($1, $2) RETURNING id, created_at";

    public static final String SQL_LIKE_POST =
        "INSERT INTO likes (user_id, post_id) VALUES ($1, $2) ON CONFLICT (user_id, post_id) DO NOTHING";

    // PostgreSQL Type OIDs for Binary Parameter Binding
    private static final int OID_INT8 = 20;
    private static final int OID_TEXT = 25;

    // Request Types in In-Flight Ring Queue
    public static final byte REQ_HEALTH      = 1;
    public static final byte REQ_FEED        = 2;
    public static final byte REQ_GET_POST    = 3;
    public static final byte REQ_INSERT_POST = 4;
    public static final byte REQ_LIKE_POST   = 5;

    // Ring Queue Capacity per PG Connection (Must be power of 2)
    public static final int QUEUE_CAPACITY = 4096;
    public static final int QUEUE_MASK     = QUEUE_CAPACITY - 1;

    // Off-heap Rx/Tx Buffer Sizes per PG Connection (512 KB each)
    public static final long RX_BUF_SIZE = 524288L;
    public static final long TX_BUF_SIZE = 524288L;

    // Callback interface invoked as completed queries arrive from Postgres
    public interface CompletionListener {
        void onHealthResult(int clientFd, int reqSeq, boolean dbOk);

        void onFeedOrPostResult(
                int clientFd, int reqSeq, byte reqType, boolean gzip,
                boolean sqlError, int rowCount,
                MemorySegment jsonBuf, int jsonLen);

        void onInsertPostResult(
                int clientFd, int reqSeq, boolean gzip, boolean sqlError,
                MemorySegment pgRxBuf, long idOff, int idLen,
                long createdOff, int createdLen,
                MemorySegment reqCopyBuf, int bodyOff, int bodyLen,
                int authorOff, int authorLen);

        void onLikePostResult(
                int clientFd, int reqSeq, boolean gzip,
                boolean fkNotFound, boolean sqlError,
                boolean insertedNew, long postId);
    }

    // =========================================================================
    // Parsed DATABASE_URL Configuration
    // =========================================================================
    public record DbEndpoint(
            String host,
            int ipv4HostOrder,
            int port,
            String user,
            String password,
            String database) {

        public static DbEndpoint parse(String databaseUrl) {
            try {
                String clean = databaseUrl.startsWith("postgresql://")
                        ? "http://" + databaseUrl.substring("postgresql://".length())
                        : databaseUrl.startsWith("postgres://")
                        ? "http://" + databaseUrl.substring("postgres://".length())
                        : databaseUrl;
                URI uri = URI.create(clean);
                String host = uri.getHost() != null ? uri.getHost() : "127.0.0.1";
                int port = uri.getPort() > 0 ? uri.getPort() : 5432;
                String path = uri.getPath();
                String db = (path != null && path.length() > 1) ? path.substring(1) : "feed";
                String user = "feed";
                String pass = "";
                String userInfo = uri.getRawUserInfo();
                if (userInfo != null && !userInfo.isEmpty()) {
                    int colon = userInfo.indexOf(':');
                    if (colon >= 0) {
                        user = userInfo.substring(0, colon);
                        pass = userInfo.substring(colon + 1);
                    } else {
                        user = userInfo;
                    }
                }
                byte[] addrBytes = InetAddress.getByName(host).getAddress();
                int ipv4 = ((addrBytes[0] & 0xFF) << 24)
                         | ((addrBytes[1] & 0xFF) << 16)
                         | ((addrBytes[2] & 0xFF) << 8)
                         |  (addrBytes[3] & 0xFF);
                return new DbEndpoint(host, ipv4, port, user, pass, db);
            } catch (Exception e) {
                throw new IllegalArgumentException("Invalid DATABASE_URL: " + databaseUrl, e);
            }
        }
    }

    // =========================================================================
    // Single Non-Blocking Pipelined PostgreSQL Connection
    // =========================================================================
    public static final class PgConn {
        public final int connIndex;
        public final boolean isWriteConn;
        public final int fd;
        public boolean epollOutArmed = false;

        // Off-heap network buffers
        public final MemorySegment rxBuf;
        public int rxReadPos = 0;
        public int rxWritePos = 0;

        public final MemorySegment txBuf;
        public int txReadPos = 0;
        public int txWritePos = 0;

        // Tracks whether Bind+Execute frames have been written since the last Sync
        public int unsyncedQueries = 0;
        public int inFlightSyncs   = 0;

        // Off-heap scratch where POST /posts body + author strings and RETURNING data are stashed while in-flight
        public final MemorySegment payloadRing;
        public int payloadWritePos = 0;
        private static final int PAYLOAD_RING_SIZE = 1048576; // 1 MB

        // In-Flight Request Ring Queue (Zero-Allocation Parallel Primitive Arrays)
        // Two-cursor gate: head is advanced as messages stream in; commitHead advances only on ReadyForQuery ('Z')
        public int head = 0;
        public int commitHead = 0;
        public int tail = 0;
        public final int[]     qClientFd      = new int[QUEUE_CAPACITY];
        public final int[]     qReqSeq        = new int[QUEUE_CAPACITY];
        public final byte[]    qReqType       = new byte[QUEUE_CAPACITY];
        public final boolean[] qGzip          = new boolean[QUEUE_CAPACITY];
        public final boolean[] qHasSync       = new boolean[QUEUE_CAPACITY];
        public final long[]    qPostId        = new long[QUEUE_CAPACITY];
        public final int[]     qBodyOff       = new int[QUEUE_CAPACITY];
        public final int[]     qBodyLen       = new int[QUEUE_CAPACITY];
        public final int[]     qAuthorOff     = new int[QUEUE_CAPACITY];
        public final int[]     qAuthorLen     = new int[QUEUE_CAPACITY];
        public final int[]     qRetIdOff      = new int[QUEUE_CAPACITY];
        public final int[]     qRetIdLen      = new int[QUEUE_CAPACITY];
        public final int[]     qRetCreatedOff = new int[QUEUE_CAPACITY];
        public final int[]     qRetCreatedLen = new int[QUEUE_CAPACITY];
        public final boolean[] qInserted      = new boolean[QUEUE_CAPACITY];
        public final boolean[] qSqlError      = new boolean[QUEUE_CAPACITY];
        public final boolean[] qFkNotFound    = new boolean[QUEUE_CAPACITY];
        public final int[]     qRowCount      = new int[QUEUE_CAPACITY];

        // Per-request state accumulated across DataRow / CommandComplete / ErrorResponse messages
        private int     curRowCount   = 0;
        private long    curJsonWrite  = Out.BODY_OFFSET;
        private boolean curSqlError   = false;
        private boolean curFkNotFound = false;
        private boolean curInserted   = false;
        private long    curIdOff      = 0L;
        private int     curIdLen      = 0;
        private long    curCreatedOff = 0L;
        private int     curCreatedLen = 0;

        public PgConn(int connIndex, boolean isWriteConn, int fd, Arena bufferArena) {
            this.connIndex   = connIndex;
            this.isWriteConn = isWriteConn;
            this.fd          = fd;
            this.rxBuf       = bufferArena.allocate(RX_BUF_SIZE, 64L);
            this.txBuf       = bufferArena.allocate(TX_BUF_SIZE, 64L);
            this.payloadRing = bufferArena.allocate(PAYLOAD_RING_SIZE, 8L);
        }

        public int inFlightCount() {
            return tail - commitHead;
        }

        public boolean canEnqueue(int extraTxBytes) {
            return (tail - commitHead) < (QUEUE_CAPACITY - 8)
                && (txWritePos + extraTxBytes + 16 <= (int) TX_BUF_SIZE);
        }

        // =====================================================================
        // Startup Handshake, Auth (Trust/MD5/SCRAM-SHA-256), & Statement Parse
        // =====================================================================
        public static int connectAndPrepareBlocking(DbEndpoint ep, MemorySegment scratch16B) throws Exception {
            // Open the raw FFM socket FD directly and run blocking handshake + Parse before allocating permanent Arena.global() buffers.
            int sockFd = Sys.socket(Sys.AF_INET, Sys.SOCK_STREAM | Sys.SOCK_NONBLOCK | Sys.SOCK_CLOEXEC, 0);
            if (sockFd < 0) {
                throw new IllegalStateException("Pg socket() failed");
            }
            Sys.setsockoptInt(sockFd, Sys.IPPROTO_TCP, Sys.TCP_NODELAY, 1, scratch16B);
            Sys.setsockoptInt(sockFd, Sys.SOL_SOCKET, Sys.SO_SNDBUF, 1048576, scratch16B);
            Sys.setsockoptInt(sockFd, Sys.SOL_SOCKET, Sys.SO_RCVBUF, 1048576, scratch16B);

            int rc = Sys.connectIpv4(sockFd, ep.ipv4HostOrder(), ep.port(), scratch16B);
            if (rc < 0) {
                int err = Sys.getErrno(Sys.errnoPtr());
                if (err != Sys.EINPROGRESS) {
                    Sys.close(sockFd);
                    throw new IllegalStateException("Pg connect() failed immediately, errno=" + err);
                }
            }
            try {
                // Send StartupMessage (Protocol v3.0 = 196608) with TimeZone=UTC and DateStyle=ISO, MDY
                ByteBuffer startBuf = ByteBuffer.allocate(512);
                startBuf.putInt(0); // length placeholder
                startBuf.putInt(196608); // v3.0
                putCString(startBuf, "user");             putCString(startBuf, ep.user());
                putCString(startBuf, "database");         putCString(startBuf, ep.database());
                putCString(startBuf, "TimeZone");         putCString(startBuf, "UTC");
                putCString(startBuf, "DateStyle");        putCString(startBuf, "ISO, MDY");
                putCString(startBuf, "client_encoding");  putCString(startBuf, "UTF8");
                startBuf.put((byte) 0);
                startBuf.putInt(0, startBuf.position());
                sendBlockingAll(sockFd, startBuf.array(), 0, startBuf.position());

                // Process Authentication & Startup ReadyForQuery
                String scramClientFirstBare = null;
                String scramClientNonce = null;

                handshakeLoop:
                while (true) {
                    byte type = readByteBlocking(sockFd);
                    int len = readIntBlocking(sockFd) - 4;
                    byte[] body = readBytesBlocking(sockFd, len);
                    ByteBuffer bb = ByteBuffer.wrap(body);

                    switch (type) {
                        case 'R' -> {
                            int authType = bb.getInt();
                            if (authType == 0) {
                                // AuthenticationOk
                            } else if (authType == 3) {
                                // CleartextPassword
                                byte[] pw = ep.password().getBytes(StandardCharsets.UTF_8);
                                ByteBuffer pMsg = ByteBuffer.allocate(1 + 4 + pw.length + 1);
                                pMsg.put((byte) 'p');
                                pMsg.putInt(4 + pw.length + 1);
                                pMsg.put(pw);
                                pMsg.put((byte) 0);
                                sendBlockingAll(sockFd, pMsg.array(), 0, pMsg.position());
                            } else if (authType == 5) {
                                // MD5Password
                                byte[] salt = new byte[4];
                                bb.get(salt);
                                byte[] md5Resp = computeMd5Password(ep.user(), ep.password(), salt);
                                ByteBuffer pMsg = ByteBuffer.allocate(1 + 4 + md5Resp.length + 1);
                                pMsg.put((byte) 'p');
                                pMsg.putInt(4 + md5Resp.length + 1);
                                pMsg.put(md5Resp);
                                pMsg.put((byte) 0);
                                sendBlockingAll(sockFd, pMsg.array(), 0, pMsg.position());
                            } else if (authType == 10) {
                                // SASL (SCRAM-SHA-256)
                                byte[] nonceBytes = new byte[18];
                                new SecureRandom().nextBytes(nonceBytes);
                                scramClientNonce = Base64.getEncoder().encodeToString(nonceBytes);
                                scramClientFirstBare = "n=,r=" + scramClientNonce;
                                byte[] clientFirstMsg = ("n,," + scramClientFirstBare).getBytes(StandardCharsets.UTF_8);
                                byte[] mech = "SCRAM-SHA-256\0".getBytes(StandardCharsets.US_ASCII);

                                ByteBuffer saslInit = ByteBuffer.allocate(1 + 4 + mech.length + 4 + clientFirstMsg.length);
                                saslInit.put((byte) 'p');
                                saslInit.putInt(4 + mech.length + 4 + clientFirstMsg.length);
                                saslInit.put(mech);
                                saslInit.putInt(clientFirstMsg.length);
                                saslInit.put(clientFirstMsg);
                                sendBlockingAll(sockFd, saslInit.array(), 0, saslInit.position());
                            } else if (authType == 11) {
                                // SASLContinue
                                String serverFirst = new String(body, 4, body.length - 4, StandardCharsets.UTF_8);
                                byte[] clientFinal = computeScramClientFinal(
                                    ep.password(), scramClientFirstBare, scramClientNonce, serverFirst
                                );
                                ByteBuffer saslResp = ByteBuffer.allocate(1 + 4 + clientFinal.length);
                                saslResp.put((byte) 'p');
                                saslResp.putInt(4 + clientFinal.length);
                                saslResp.put(clientFinal);
                                sendBlockingAll(sockFd, saslResp.array(), 0, saslResp.position());
                            } else if (authType == 12) {
                                // SASLFinal -> verified
                            } else {
                                throw new IllegalStateException("Unsupported PG auth type: " + authType);
                            }
                        }
                        case 'Z' -> {
                            break handshakeLoop;
                        }
                        case 'E' -> {
                            throw new IllegalStateException("PG Startup Error: " + parseErrorString(body));
                        }
                        default -> {
                            // Ignore 'S' (ParameterStatus), 'K' (BackendKeyData), 'N' (Notice)
                        }
                    }
                }

                // 2. Send Parse messages for all 5 prepared statements + Sync in a single batch
                ByteBuffer parseBatch = ByteBuffer.allocate(2048);
                appendParseMessage(parseBatch, "h", SQL_HEALTH);
                appendParseMessage(parseBatch, "f", SQL_FEED);
                appendParseMessage(parseBatch, "p", SQL_GET_POST, OID_INT8);
                appendParseMessage(parseBatch, "i", SQL_INSERT_POST, OID_INT8, OID_TEXT);
                appendParseMessage(parseBatch, "l", SQL_LIKE_POST, OID_INT8, OID_INT8);
                // Append Sync ('S', len=4)
                parseBatch.put((byte) 'S');
                parseBatch.putInt(4);
                sendBlockingAll(sockFd, parseBatch.array(), 0, parseBatch.position());

                // Wait for ParseComplete ('1' x 5) and ReadyForQuery ('Z')
                while (true) {
                    byte type = readByteBlocking(sockFd);
                    int len = readIntBlocking(sockFd) - 4;
                    byte[] body = readBytesBlocking(sockFd, len);
                    if (type == 'E') {
                        throw new IllegalStateException("PG Parse Error: " + parseErrorString(body));
                    }
                    if (type == 'Z') {
                        break;
                    }
                }
                return sockFd;
            } catch (Exception e) {
                Sys.close(sockFd);
                throw e;
            }
        }

        // =====================================================================
        // Zero-Allocation Pipelined Query Enqueue Methods
        // =====================================================================

        /**
         * Enqueues GET /feed: Bind(portal="", stmt="f", 0 params, 1 text result format) + Execute("", 0).
         * Total wire size: 26 bytes.
         */
        public void enqueueFeed(int clientFd, int reqSeq, boolean gzip) {
            int p = txWritePos;
            // Bind ('B', len=15, portal="", stmt="f", numFmt=0, numParams=0, numResFmt=1, resFmt[0]=0 [Text])
            txBuf.set(JAVA_BYTE, p, (byte) 'B');
            txBuf.set(JAVA_INT_UNALIGNED, p + 1L, Integer.reverseBytes(15));
            txBuf.set(JAVA_BYTE, p + 5L, (byte) 0);   // portal ""
            txBuf.set(JAVA_BYTE, p + 6L, (byte) 'f'); // stmt "f"
            txBuf.set(JAVA_BYTE, p + 7L, (byte) 0);
            txBuf.set(JAVA_INT_UNALIGNED, p + 8L, 0); // 0 param formats, 0 params
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 12L, Short.reverseBytes((short) 1)); // 1 result format
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 14L, (short) 0); // format 0 = Text for all columns

            // Execute ('E', len=9, portal="", maxRows=0)
            txBuf.set(JAVA_BYTE, p + 16L, (byte) 'E');
            txBuf.set(JAVA_INT_UNALIGNED, p + 17L, Integer.reverseBytes(9));
            txBuf.set(JAVA_BYTE, p + 21L, (byte) 0);
            txBuf.set(JAVA_INT_UNALIGNED, p + 22L, 0);

            txWritePos = p + 26;
            unsyncedQueries++;

            int idx = tail & QUEUE_MASK;
            qClientFd[idx] = clientFd;
            qReqSeq[idx]   = reqSeq;
            qReqType[idx]  = REQ_FEED;
            qGzip[idx]     = gzip;
            qHasSync[idx]  = false;
            tail++;
        }

        /**
         * Enqueues GET /posts/:id: Bind(portal="", stmt="p", $1=binary int8 postId, Text output) + Execute.
         * Total wire size: 40 bytes.
         */
        public void enqueueGetPost(int clientFd, int reqSeq, boolean gzip, long postId) {
            int p = txWritePos;
            // Bind ('B', len=29, portal="", stmt="p", 1 paramFmt=1 [Binary], 1 param [len=8, postId], 1 resFmt=0)
            txBuf.set(JAVA_BYTE, p, (byte) 'B');
            txBuf.set(JAVA_INT_UNALIGNED, p + 1L, Integer.reverseBytes(29));
            txBuf.set(JAVA_BYTE, p + 5L, (byte) 0);
            txBuf.set(JAVA_BYTE, p + 6L, (byte) 'p');
            txBuf.set(JAVA_BYTE, p + 7L, (byte) 0);
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 8L, Short.reverseBytes((short) 1));  // 1 format code
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 10L, Short.reverseBytes((short) 1)); // Binary (1)
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 12L, Short.reverseBytes((short) 1)); // 1 parameter
            txBuf.set(JAVA_INT_UNALIGNED, p + 14L, Integer.reverseBytes(8));         // 8 bytes
            txBuf.set(JAVA_LONG_UNALIGNED, p + 18L, Long.reverseBytes(postId));      // $1 = postId
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 26L, Short.reverseBytes((short) 1)); // 1 result format
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 28L, (short) 0);                     // Text (0)

            // Execute ('E', len=9, portal="", maxRows=0)
            txBuf.set(JAVA_BYTE, p + 30L, (byte) 'E');
            txBuf.set(JAVA_INT_UNALIGNED, p + 31L, Integer.reverseBytes(9));
            txBuf.set(JAVA_BYTE, p + 35L, (byte) 0);
            txBuf.set(JAVA_INT_UNALIGNED, p + 36L, 0);

            txWritePos = p + 40;
            unsyncedQueries++;

            int idx = tail & QUEUE_MASK;
            qClientFd[idx] = clientFd;
            qReqSeq[idx]   = reqSeq;
            qReqType[idx]  = REQ_GET_POST;
            qGzip[idx]     = gzip;
            qPostId[idx]   = postId;
            qHasSync[idx]  = false;
            tail++;
        }

        /**
         * Enqueues GET /health: Bind(portal="", stmt="h") + Execute. Total wire size: 26 bytes.
         */
        public void enqueueHealth(int clientFd, int reqSeq) {
            int p = txWritePos;
            txBuf.set(JAVA_BYTE, p, (byte) 'B');
            txBuf.set(JAVA_INT_UNALIGNED, p + 1L, Integer.reverseBytes(15));
            txBuf.set(JAVA_BYTE, p + 5L, (byte) 0);
            txBuf.set(JAVA_BYTE, p + 6L, (byte) 'h');
            txBuf.set(JAVA_BYTE, p + 7L, (byte) 0);
            txBuf.set(JAVA_INT_UNALIGNED, p + 8L, 0);
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 12L, Short.reverseBytes((short) 1));
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 14L, (short) 0);

            txBuf.set(JAVA_BYTE, p + 16L, (byte) 'E');
            txBuf.set(JAVA_INT_UNALIGNED, p + 17L, Integer.reverseBytes(9));
            txBuf.set(JAVA_BYTE, p + 21L, (byte) 0);
            txBuf.set(JAVA_INT_UNALIGNED, p + 22L, 0);

            txWritePos = p + 26;
            unsyncedQueries++;

            int idx = tail & QUEUE_MASK;
            qClientFd[idx] = clientFd;
            qReqSeq[idx]   = reqSeq;
            qReqType[idx]  = REQ_HEALTH;
            qGzip[idx]     = false;
            qHasSync[idx]  = false;
            tail++;
        }

        /**
         * Enqueues POST /posts: Bind(stmt="i", $1=binary int8 userId, $2=text body, Text output) + Execute.
         * Multiple POST /posts enqueued in the same event-loop tick share a single trailing Sync
         * (PostgreSQL Group Commit!).
         */
        public void enqueueInsertPost(
                int clientFd, int reqSeq, boolean gzip, long userId,
                MemorySegment bodySeg, long bodyOff, int bodyLen,
                MemorySegment authorSeg, long authorOff, int authorLen) {

            int bindLen = 35 + bodyLen;
            int p = txWritePos;

            txBuf.set(JAVA_BYTE, p, (byte) 'B');
            txBuf.set(JAVA_INT_UNALIGNED, p + 1L, Integer.reverseBytes(bindLen));
            txBuf.set(JAVA_BYTE, p + 5L, (byte) 0);
            txBuf.set(JAVA_BYTE, p + 6L, (byte) 'i');
            txBuf.set(JAVA_BYTE, p + 7L, (byte) 0);
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 8L, Short.reverseBytes((short) 2));  // 2 param format codes
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 10L, Short.reverseBytes((short) 1)); // $1 = Binary (int8)
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 12L, (short) 0);                     // $2 = Text (UTF-8)
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 14L, Short.reverseBytes((short) 2)); // 2 parameters
            // Param $1: 8-byte userId
            txBuf.set(JAVA_INT_UNALIGNED, p + 16L, Integer.reverseBytes(8));
            txBuf.set(JAVA_LONG_UNALIGNED, p + 20L, Long.reverseBytes(userId));
            // Param $2: bodyLen bytes
            txBuf.set(JAVA_INT_UNALIGNED, p + 28L, Integer.reverseBytes(bodyLen));
            MemorySegment.copy(bodySeg, bodyOff, txBuf, p + 32L, bodyLen);

            int afterBody = p + 32 + bodyLen;
            txBuf.set(JAVA_SHORT_UNALIGNED, afterBody, Short.reverseBytes((short) 1)); // 1 result format
            txBuf.set(JAVA_SHORT_UNALIGNED, afterBody + 2L, (short) 0);                // Text output

            // Execute ('E', len=9, portal="", maxRows=0)
            int execPos = afterBody + 4;
            txBuf.set(JAVA_BYTE, execPos, (byte) 'E');
            txBuf.set(JAVA_INT_UNALIGNED, execPos + 1L, Integer.reverseBytes(9));
            txBuf.set(JAVA_BYTE, execPos + 5L, (byte) 0);
            txBuf.set(JAVA_INT_UNALIGNED, execPos + 6L, 0);

            txWritePos = execPos + 10;
            unsyncedQueries++;

            // Stash body and author slices in payloadRing so the 201 response can echo them on completion
            if (payloadWritePos + bodyLen + authorLen + 16 > PAYLOAD_RING_SIZE) {
                payloadWritePos = 0;
            }
            int savedBodyOff = payloadWritePos;
            MemorySegment.copy(bodySeg, bodyOff, payloadRing, savedBodyOff, bodyLen);
            int savedAuthorOff = savedBodyOff + bodyLen;
            MemorySegment.copy(authorSeg, authorOff, payloadRing, savedAuthorOff, authorLen);
            payloadWritePos = savedAuthorOff + authorLen;

            int idx = tail & QUEUE_MASK;
            qClientFd[idx]  = clientFd;
            qReqSeq[idx]    = reqSeq;
            qReqType[idx]   = REQ_INSERT_POST;
            qGzip[idx]      = gzip;
            qHasSync[idx]   = false;
            qBodyOff[idx]   = savedBodyOff;
            qBodyLen[idx]   = bodyLen;
            qAuthorOff[idx] = savedAuthorOff;
            qAuthorLen[idx] = authorLen;
            tail++;
        }

        /**
         * Enqueues POST /posts/:id/like: Bind(stmt="l", $1=userId, $2=postId) + Execute.
         * Likes share the batch's trailing Sync for high-throughput group commits.
         */
        public void enqueueLikePost(int clientFd, int reqSeq, boolean gzip, long userId, long postId) {
            int p = txWritePos;
            // Bind ('B', len=39, portal="", stmt="l", 1 paramFmt=1 [Binary for all], 2 params [userId, postId], 0 resFmt)
            txBuf.set(JAVA_BYTE, p, (byte) 'B');
            txBuf.set(JAVA_INT_UNALIGNED, p + 1L, Integer.reverseBytes(39));
            txBuf.set(JAVA_BYTE, p + 5L, (byte) 0);
            txBuf.set(JAVA_BYTE, p + 6L, (byte) 'l');
            txBuf.set(JAVA_BYTE, p + 7L, (byte) 0);
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 8L, Short.reverseBytes((short) 1));  // 1 format code applies to all
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 10L, Short.reverseBytes((short) 1)); // Binary (1)
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 12L, Short.reverseBytes((short) 2)); // 2 parameters
            txBuf.set(JAVA_INT_UNALIGNED, p + 14L, Integer.reverseBytes(8));
            txBuf.set(JAVA_LONG_UNALIGNED, p + 18L, Long.reverseBytes(userId));
            txBuf.set(JAVA_INT_UNALIGNED, p + 26L, Integer.reverseBytes(8));
            txBuf.set(JAVA_LONG_UNALIGNED, p + 30L, Long.reverseBytes(postId));
            txBuf.set(JAVA_SHORT_UNALIGNED, p + 38L, (short) 0);                     // 0 result formats

            // Execute ('E', len=9)
            txBuf.set(JAVA_BYTE, p + 40L, (byte) 'E');
            txBuf.set(JAVA_INT_UNALIGNED, p + 41L, Integer.reverseBytes(9));
            txBuf.set(JAVA_BYTE, p + 45L, (byte) 0);
            txBuf.set(JAVA_INT_UNALIGNED, p + 46L, 0);

            txWritePos = p + 50;
            unsyncedQueries++;

            int idx = tail & QUEUE_MASK;
            qClientFd[idx] = clientFd;
            qReqSeq[idx]   = reqSeq;
            qReqType[idx]  = REQ_LIKE_POST;
            qGzip[idx]     = gzip;
            qPostId[idx]   = postId;
            qHasSync[idx]  = false;
            tail++;
        }

        private void appendSyncMarker() {
            int p = txWritePos;
            txBuf.set(JAVA_BYTE, p, (byte) 'S');
            txBuf.set(JAVA_INT_UNALIGNED, p + 1L, Integer.reverseBytes(4));
            txWritePos = p + 5;
            if (tail > 0) {
                qHasSync[(tail - 1) & QUEUE_MASK] = true;
            }
            unsyncedQueries = 0;
            inFlightSyncs++;
        }

        /**
         * Appends a single Sync ('S') for all unsynced Bind+Execute messages accumulated during this
         * event-loop tick and flushes the transmit buffer to the Postgres socket in one send() syscall.
         */
        public boolean flushIfNeeded(MemorySegment errnoSeg) {
            if (unsyncedQueries > 0) {
                appendSyncMarker();
            }
            if (txReadPos >= txWritePos) {
                return true;
            }
            while (txReadPos < txWritePos) {
                long sent = Sys.send(fd, txBuf, txReadPos, txWritePos - txReadPos);
                if (sent > 0) {
                    txReadPos += (int) sent;
                } else {
                    int err = Sys.getErrno(errnoSeg);
                    if (err == Sys.EINTR) continue;
                    if (err == Sys.EAGAIN) break;
                    return false;
                }
            }
            if (txReadPos > 0) {
                if (txReadPos == txWritePos) {
                    txReadPos = 0;
                    txWritePos = 0;
                } else {
                    int unwritten = txWritePos - txReadPos;
                    MemorySegment.copy(txBuf, txReadPos, txBuf, 0L, unwritten);
                    txReadPos = 0;
                    txWritePos = unwritten;
                }
            }
            return txWritePos == 0;
        }

        // =====================================================================
        // Non-Blocking Receive & Wire Protocol State Machine
        // =====================================================================

        public void onReadable(
                MemorySegment errnoSeg,
                MemorySegment jsonScratch,
                CompletionListener listener) {

            while (true) {
                // Compact rxBuf if needed
                if (rxReadPos > 0) {
                    int unread = rxWritePos - rxReadPos;
                    if (unread == 0) {
                        rxReadPos = 0;
                        rxWritePos = 0;
                    } else if (rxWritePos > (int) RX_BUF_SIZE - 32768) {
                        MemorySegment.copy(rxBuf, rxReadPos, rxBuf, 0L, unread);
                        rxReadPos = 0;
                        rxWritePos = unread;
                    }
                }

                long availSpace = RX_BUF_SIZE - rxWritePos;
                if (availSpace <= 0) break;

                long n = Sys.recv(fd, rxBuf, rxWritePos, availSpace);
                if (n > 0) {
                    rxWritePos += (int) n;
                    parseIncomingFrames(jsonScratch, listener);
                } else if (n == 0) {
                    // Connection closed by Postgres
                    break;
                } else {
                    int err = Sys.getErrno(errnoSeg);
                    if (err == Sys.EINTR) continue;
                    break; // EAGAIN
                }
            }
        }

        private void parseIncomingFrames(MemorySegment jsonScratch, CompletionListener listener) {
            int pos = rxReadPos;
            int limit = rxWritePos;

            while (pos + 5 <= limit) {
                byte msgType = rxBuf.get(JAVA_BYTE, pos);
                int msgLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, pos + 1L));
                int frameEnd = pos + 1 + msgLen;
                if (frameEnd > limit) {
                    break; // Wait for full frame
                }

                int bodyPos = pos + 5;
                int bodyLen = msgLen - 4;

                switch (msgType) {
                    case '2' -> {
                        // BindComplete: initialize state for the current head request
                        int idx = head & QUEUE_MASK;
                        byte rt = qReqType[idx];
                        curRowCount   = 0;
                        curSqlError   = false;
                        curFkNotFound = false;
                        curInserted   = false;
                        if (rt == REQ_FEED) {
                            curJsonWrite = Json.startFeedObject(jsonScratch, Out.BODY_OFFSET);
                        } else if (rt == REQ_GET_POST) {
                            curJsonWrite = Json.startSinglePostObject(jsonScratch, Out.BODY_OFFSET);
                        }
                    }
                    case 'D' -> {
                        // DataRow
                        int idx = head & QUEUE_MASK;
                        byte rt = qReqType[idx];
                        if (rt == REQ_FEED || rt == REQ_GET_POST) {
                            // 5 columns: id (0), body (1), created_at (2), username (3), like_count (4)
                            int cPos = bodyPos + 2; // skip int16 column count (5)
                            int idLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, cPos));
                            long idOff = cPos + 4L;
                            cPos = (int) idOff + idLen;

                            int pBodyLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, cPos));
                            long pBodyOff = cPos + 4L;
                            cPos = (int) pBodyOff + pBodyLen;

                            int createdLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, cPos));
                            long createdOff = cPos + 4L;
                            cPos = (int) createdOff + createdLen;

                            int authorLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, cPos));
                            long authorOff = cPos + 4L;
                            cPos = (int) authorOff + authorLen;

                            int likesLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, cPos));
                            long likesOff = cPos + 4L;

                            curJsonWrite = Json.writePostFromPgText(
                                jsonScratch, curJsonWrite, curRowCount > 0,
                                rxBuf,
                                idOff, idLen,
                                pBodyOff, pBodyLen,
                                createdOff, createdLen,
                                authorOff, authorLen,
                                likesOff, likesLen
                            );
                            curRowCount++;
                        } else if (rt == REQ_INSERT_POST) {
                            // 2 columns from RETURNING: id (0), created_at (1)
                            int cPos = bodyPos + 2;
                            int idLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, cPos));
                            long idOff = cPos + 4L;
                            cPos = (int) idOff + idLen;

                            int createdLen = Integer.reverseBytes(rxBuf.get(JAVA_INT_UNALIGNED, cPos));
                            long createdOff = cPos + 4L;

                            // Stash returning id and created_at into payloadRing so they survive until ReadyForQuery ('Z')
                            if (payloadWritePos + idLen + createdLen + 16 > PAYLOAD_RING_SIZE) {
                                payloadWritePos = 0;
                            }
                            int sIdOff = payloadWritePos;
                            MemorySegment.copy(rxBuf, idOff, payloadRing, sIdOff, idLen);
                            payloadWritePos += idLen;

                            int sCreatedOff = payloadWritePos;
                            MemorySegment.copy(rxBuf, createdOff, payloadRing, sCreatedOff, createdLen);
                            payloadWritePos += createdLen;

                            qRetIdOff[idx]      = sIdOff;
                            qRetIdLen[idx]      = idLen;
                            qRetCreatedOff[idx] = sCreatedOff;
                            qRetCreatedLen[idx] = createdLen;
                            qRowCount[idx]      = 1;
                        } else if (rt == REQ_HEALTH) {
                            curRowCount++;
                        }
                    }
                    case 'C' -> {
                        // CommandComplete: query finished
                        int idx = head & QUEUE_MASK;
                        byte rt = qReqType[idx];
                        if (isWriteConn) {
                            if (rt == REQ_LIKE_POST) {
                                // Tag is "INSERT 0 1\0" (len=11) or "INSERT 0 0\0" (len=11)
                                if (bodyLen >= 10) {
                                    byte lastDigit = rxBuf.get(JAVA_BYTE, bodyPos + 9L);
                                    qInserted[idx] = (lastDigit == '1');
                                }
                                qFkNotFound[idx] = curFkNotFound;
                            }
                            qSqlError[idx] = curSqlError;
                            // Advance parse pointer: DO NOT respond to client yet; hold until ReadyForQuery ('Z')!
                            head++;
                            curRowCount = 0;
                            curSqlError = false;
                            curFkNotFound = false;
                        } else {
                            // Reader connection: respond immediately on CommandComplete
                            completeReaderRequest(jsonScratch, listener);
                        }
                    }
                    case 'E' -> {
                        // ErrorResponse: failed statement does not get CommandComplete ('C')
                        curSqlError = true;
                        curFkNotFound = isForeignKeyViolation(rxBuf, bodyPos, bodyLen);
                        if (isWriteConn) {
                            if (head != tail) {
                                int idx = head & QUEUE_MASK;
                                qSqlError[idx] = true;
                                qFkNotFound[idx] = curFkNotFound;
                                head++;
                            }
                            curRowCount = 0;
                        } else {
                            if (head != tail) {
                                completeReaderRequest(jsonScratch, listener);
                            }
                        }
                    }
                    case 'Z' -> {
                        // ReadyForQuery: closes the current Sync batch (PostgreSQL has flushed WAL / committed on disk)
                        inFlightSyncs--;
                        if (isWriteConn) {
                            // If an earlier statement in the batch failed, PostgreSQL aborted subsequent statements until Sync
                            if (curSqlError) {
                                while (head != tail) {
                                    int idx = head & QUEUE_MASK;
                                    boolean lastInBatch = qHasSync[idx];
                                    qSqlError[idx] = true;
                                    head++;
                                    if (lastInBatch) break;
                                }
                            }
                            while (commitHead != head) {
                                int idx = commitHead & QUEUE_MASK;
                                int clientFd = qClientFd[idx];
                                int reqSeq   = qReqSeq[idx];
                                byte rt      = qReqType[idx];
                                boolean gzip = qGzip[idx];
                                boolean hasSync = qHasSync[idx];
                                commitHead++;

                                if (rt == REQ_INSERT_POST) {
                                    listener.onInsertPostResult(
                                        clientFd, reqSeq, gzip,
                                        qSqlError[idx] || qRowCount[idx] == 0,
                                        payloadRing, (long) qRetIdOff[idx], qRetIdLen[idx],
                                        (long) qRetCreatedOff[idx], qRetCreatedLen[idx],
                                        payloadRing, qBodyOff[idx], qBodyLen[idx],
                                        qAuthorOff[idx], qAuthorLen[idx]
                                    );
                                } else if (rt == REQ_LIKE_POST) {
                                    listener.onLikePostResult(
                                        clientFd, reqSeq, gzip,
                                        qFkNotFound[idx],
                                        qSqlError[idx] && !qFkNotFound[idx],
                                        qInserted[idx],
                                        qPostId[idx]
                                    );
                                }
                                if (hasSync) break;
                            }
                        } else {
                            while (head != tail) {
                                boolean lastInBatch = qHasSync[head & QUEUE_MASK];
                                completeReaderRequest(jsonScratch, listener);
                                if (lastInBatch) break;
                                curSqlError = true;
                            }
                        }
                        curSqlError = false;
                        curFkNotFound = false;
                    }
                    default -> {
                        // Ignore '1' (ParseComplete), 'n' (NoData), 'S', 'N'
                    }
                }

                pos = frameEnd;
            }

            rxReadPos = pos;
        }

        private void completeReaderRequest(MemorySegment jsonScratch, CompletionListener listener) {
            int idx = head & QUEUE_MASK;
            int clientFd = qClientFd[idx];
            int reqSeq   = qReqSeq[idx];
            byte rt      = qReqType[idx];
            boolean gzip = qGzip[idx];
            head++;
            commitHead = head;

            switch (rt) {
                case REQ_FEED -> {
                    long endPos = Json.endFeedObject(jsonScratch, curJsonWrite);
                    int jsonLen = (int) (endPos - Out.BODY_OFFSET);
                    listener.onFeedOrPostResult(
                        clientFd, reqSeq, rt, gzip, curSqlError, curRowCount, jsonScratch, jsonLen
                    );
                }
                case REQ_GET_POST -> {
                    long endPos = Json.endSinglePostObject(jsonScratch, curJsonWrite);
                    int jsonLen = (int) (endPos - Out.BODY_OFFSET);
                    listener.onFeedOrPostResult(
                        clientFd, reqSeq, rt, gzip, curSqlError, curRowCount, jsonScratch, jsonLen
                    );
                }
                case REQ_HEALTH -> {
                    listener.onHealthResult(clientFd, reqSeq, !curSqlError && curRowCount > 0);
                }
            }

            // Reset per-request state for safety
            curRowCount   = 0;
            curJsonWrite  = Out.BODY_OFFSET;
            curSqlError   = false;
            curFkNotFound = false;
        }

        private static boolean isForeignKeyViolation(MemorySegment buf, int pos, int len) {
            int end = pos + len;
            int i = pos;
            while (i < end) {
                byte fieldType = buf.get(JAVA_BYTE, i++);
                if (fieldType == 0) break;
                int valStart = i;
                while (i < end && buf.get(JAVA_BYTE, i) != 0) i++;
                int valLen = i - valStart;
                i++; // skip null terminator
                if (fieldType == 'C' && valLen == 5) {
                    if (buf.get(JAVA_BYTE, valStart)     == '2' &&
                        buf.get(JAVA_BYTE, valStart + 1) == '3' &&
                        buf.get(JAVA_BYTE, valStart + 2) == '5' &&
                        buf.get(JAVA_BYTE, valStart + 3) == '0' &&
                        buf.get(JAVA_BYTE, valStart + 4) == '3') {
                        return true;
                    }
                }
            }
            return false;
        }

        // =====================================================================
        // Startup Blocking I/O & Auth Helpers (Used Only Once at Boot)
        // =====================================================================

        private static void sendBlockingAll(int fd, byte[] data, int off, int len) {
            MemorySegment errnoSeg = Sys.errnoPtr();
            try (Arena a = Arena.ofConfined()) {
                MemorySegment seg = a.allocate(len);
                MemorySegment.copy(data, off, seg, JAVA_BYTE, 0L, len);
                int sent = 0;
                while (sent < len) {
                    long n = Sys.send(fd, seg, sent, len - sent);
                    if (n > 0) {
                        sent += (int) n;
                    } else {
                        int err = Sys.getErrno(errnoSeg);
                        if (err == Sys.EAGAIN || err == Sys.EINPROGRESS || err == Sys.EINTR) {
                            Thread.onSpinWait();
                            continue;
                        }
                        throw new IllegalStateException("PG startup send failed, errno=" + err);
                    }
                }
            }
        }

        private static byte readByteBlocking(int fd) {
            return readBytesBlocking(fd, 1)[0];
        }

        private static int readIntBlocking(int fd) {
            byte[] b = readBytesBlocking(fd, 4);
            return ((b[0] & 0xFF) << 24)
                 | ((b[1] & 0xFF) << 16)
                 | ((b[2] & 0xFF) << 8)
                 |  (b[3] & 0xFF);
        }

        private static byte[] readBytesBlocking(int fd, int len) {
            MemorySegment errnoSeg = Sys.errnoPtr();
            byte[] out = new byte[len];
            try (Arena a = Arena.ofConfined()) {
                MemorySegment seg = a.allocate(Math.max(1, len));
                int got = 0;
                while (got < len) {
                    long n = Sys.recv(fd, seg, got, len - got);
                    if (n > 0) {
                        got += (int) n;
                    } else if (n == 0) {
                        throw new IllegalStateException("PG closed socket during startup handshake");
                    } else {
                        int err = Sys.getErrno(errnoSeg);
                        if (err == Sys.EAGAIN || err == Sys.EINTR) {
                            Thread.onSpinWait();
                            continue;
                        }
                        throw new IllegalStateException("PG startup recv failed, errno=" + err);
                    }
                }
                MemorySegment.copy(seg, JAVA_BYTE, 0L, out, 0, len);
            }
            return out;
        }
    }

    // =========================================================================
    // Startup Packet Builders & SCRAM / MD5 Helpers
    // =========================================================================

    private static void appendParseMessage(ByteBuffer buf, String stmtName, String sql, int... paramOids) {
        byte[] nameBytes = stmtName.getBytes(StandardCharsets.US_ASCII);
        byte[] sqlBytes = sql.getBytes(StandardCharsets.UTF_8);
        int len = 4 + (nameBytes.length + 1) + (sqlBytes.length + 1) + 2 + (paramOids.length * 4);
        buf.put((byte) 'P');
        buf.putInt(len);
        buf.put(nameBytes).put((byte) 0);
        buf.put(sqlBytes).put((byte) 0);
        buf.putShort((short) paramOids.length);
        for (int oid : paramOids) {
            buf.putInt(oid);
        }
    }

    private static void putCString(ByteBuffer buf, String s) {
        buf.put(s.getBytes(StandardCharsets.UTF_8));
        buf.put((byte) 0);
    }

    private static String parseErrorString(byte[] body) {
        StringBuilder sb = new StringBuilder();
        int i = 0;
        while (i < body.length && body[i] != 0) {
            char code = (char) body[i++];
            int start = i;
            while (i < body.length && body[i] != 0) i++;
            String val = new String(body, start, i - start, StandardCharsets.UTF_8);
            i++;
            sb.append(code).append('=').append(val).append(' ');
        }
        return sb.toString();
    }

    private static byte[] computeMd5Password(String user, String password, byte[] salt) throws Exception {
        MessageDigest md = MessageDigest.getInstance("MD5");
        md.update(password.getBytes(StandardCharsets.UTF_8));
        md.update(user.getBytes(StandardCharsets.UTF_8));
        byte[] innerHex = bytesToHexAscii(md.digest());
        md.reset();
        md.update(innerHex);
        md.update(salt);
        byte[] outerHex = bytesToHexAscii(md.digest());
        byte[] res = new byte[3 + outerHex.length];
        res[0] = 'm'; res[1] = 'd'; res[2] = '5';
        System.arraycopy(outerHex, 0, res, 3, outerHex.length);
        return res;
    }

    private static byte[] bytesToHexAscii(byte[] bytes) {
        byte[] hex = "0123456789abcdef".getBytes(StandardCharsets.US_ASCII);
        byte[] out = new byte[bytes.length * 2];
        for (int i = 0; i < bytes.length; i++) {
            int v = bytes[i] & 0xFF;
            out[i * 2]     = hex[v >>> 4];
            out[i * 2 + 1] = hex[v & 0x0F];
        }
        return out;
    }

    private static byte[] computeScramClientFinal(
            String password, String clientFirstBare, String clientNonce, String serverFirst) throws Exception {

        String fullNonce = null;
        byte[] salt = null;
        int iterations = 4096;

        for (String part : serverFirst.split(",")) {
            if (part.startsWith("r=")) {
                fullNonce = part.substring(2);
            } else if (part.startsWith("s=")) {
                salt = Base64.getDecoder().decode(part.substring(2));
            } else if (part.startsWith("i=")) {
                iterations = Integer.parseInt(part.substring(2));
            }
        }
        if (fullNonce == null || !fullNonce.startsWith(clientNonce) || salt == null) {
            throw new IllegalStateException("Invalid SCRAM server-first-message: " + serverFirst);
        }

        SecretKeyFactory skf = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256");
        PBEKeySpec spec = new PBEKeySpec(password.toCharArray(), salt, iterations, 256);
        byte[] saltedPassword = skf.generateSecret(spec).getEncoded();

        Mac hmac = Mac.getInstance("HmacSHA256");
        hmac.init(new SecretKeySpec(saltedPassword, "HmacSHA256"));
        byte[] clientKey = hmac.doFinal("Client Key".getBytes(StandardCharsets.UTF_8));

        byte[] storedKey = MessageDigest.getInstance("SHA-256").digest(clientKey);

        String clientFinalWithoutProof = "c=biws,r=" + fullNonce;
        String authMessage = clientFirstBare + "," + serverFirst + "," + clientFinalWithoutProof;

        hmac.init(new SecretKeySpec(storedKey, "HmacSHA256"));
        byte[] clientSignature = hmac.doFinal(authMessage.getBytes(StandardCharsets.UTF_8));

        byte[] clientProof = new byte[clientKey.length];
        for (int i = 0; i < clientKey.length; i++) {
            clientProof[i] = (byte) (clientKey[i] ^ clientSignature[i]);
        }

        String clientFinal = clientFinalWithoutProof + ",p=" + Base64.getEncoder().encodeToString(clientProof);
        return clientFinal.getBytes(StandardCharsets.UTF_8);
    }
}
