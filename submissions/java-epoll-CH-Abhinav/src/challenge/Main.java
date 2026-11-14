package challenge;

import java.lang.foreign.Arena;
import java.lang.foreign.MemorySegment;
import java.nio.charset.StandardCharsets;
import java.util.Arrays;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

import static java.lang.foreign.ValueLayout.JAVA_BYTE;

public final class Main {

    private Main() {}

    public static void main(String[] args) throws Exception {
        // 1. Read configuration from environment variables
        String dbUrl = envOrDefault(
            "DATABASE_URL",
            "postgres://feed:feed@127.0.0.1:5432/feed"
        );
        String jwtSecret = envOrDefault(
            "JWT_SECRET",
            "challenge-secret-key-change-in-production"
        );
        int[] listenPorts = resolveListenPorts();

        // 2. Initialize HMAC-SHA256 IPAD/OPAD midstates & run cryptographic self-test
        Jwt.init(jwtSecret);

        // 3. Force class initialization of Json, Out, and Sys and run C2 Tier-4 JIT Warmup
        warmupHotPaths(jwtSecret);

        // 4. Parse PostgreSQL endpoint
        Pg.DbEndpoint dbEndpoint = Pg.DbEndpoint.parse(dbUrl);

        // 5. Compute optimal worker count and total DB pool sizes (88 total conns across box)
        // Defaulting to 64 reads and 24 writes stays safely below both the benchmark box limit (Rule 5: 512 max)
        // and standard PostgreSQL CI environments (max_connections = 100).
        int numWorkers = Math.max(1, Runtime.getRuntime().availableProcessors());
        int totalTargetReads  = Integer.parseInt(envOrDefault("PG_TOTAL_READ_CONNS", "64"));
        int totalTargetWrites = Integer.parseInt(envOrDefault("PG_TOTAL_WRITE_CONNS", "24"));

        int readConnsPerWorker  = Math.max(1, (totalTargetReads + numWorkers - 1) / numWorkers);
        int writeConnsPerWorker = Math.max(1, (totalTargetWrites + numWorkers - 1) / numWorkers);

        // 6. Allocate shared 8 MB FD State Table (2,097,152 FDs * 4 bytes) off-heap
        Arena globalArena = Arena.global();
        MemorySegment fdStateTable = globalArena.allocate((long) Loop.MAX_FD * 4L, 64L);
        fdStateTable.fill((byte) 0);

        MemorySegment bootTimespec = globalArena.allocate(16L, 8L);
        long serverStartEpochSec = Sys.clockRealTimeCoarseSec(bootTimespec);

        // 7. Initialize all worker Loops (with retry backoff if Postgres is still booting)
        Loop[] loops = new Loop[numWorkers];
        for (int w = 0; w < numWorkers; w++) {
            loops[w] = createLoopWithRetry(
                w, listenPorts, serverStartEpochSec,
                fdStateTable, dbEndpoint, readConnsPerWorker, writeConnsPerWorker
            );
        }

        System.out.printf(
            "Server ready: workers=%d, ports=%s, pgReadPerWorker=%d, pgWritePerWorker=%d%n",
            numWorkers, Arrays.toString(listenPorts), readConnsPerWorker, writeConnsPerWorker
        );

        // 8. Spawn platform threads for Workers 1..N-1 and run Worker 0 on main thread
        for (int w = 1; w < numWorkers; w++) {
            final Loop workerLoop = loops[w];
            Thread t = new Thread(workerLoop, "epoll-worker-" + w);
            t.setDaemon(false);
            t.start();
        }

        // Run Worker 0 directly on the main thread
        loops[0].run();
    }

    private static Loop createLoopWithRetry(
            int workerId,
            int[] listenPorts,
            long serverStartEpochSec,
            MemorySegment fdStateTable,
            Pg.DbEndpoint dbEndpoint,
            int readConnsPerWorker,
            int writeConnsPerWorker) throws Exception {

        int attempts = 0;
        while (true) {
            try {
                return new Loop(
                    workerId, listenPorts, serverStartEpochSec,
                    fdStateTable, dbEndpoint, readConnsPerWorker, writeConnsPerWorker
                );
            } catch (Exception e) {
                attempts++;
                if (attempts >= 30) {
                    throw e;
                }
                Thread.sleep(200L);
            }
        }
    }

    /**
     * Exercises all hot execution paths (JWT verification, JSON parsing/escaping, timestamp
     * formatting, and ISA-L AVX-512 Gzip framing) 20,000 times inside a temporary off-heap
     * Arena so HotSpot C2 compiles every method to native assembly before sockets open.
     */
    private static void warmupHotPaths(String secret) throws Exception {
        String hdr = "eyJhbGciOiJIUzI1NiIsInR5cCI6IkpXVCJ9";
        String pay = java.util.Base64.getUrlEncoder().withoutPadding()
                .encodeToString("{\"sub\":\"123456\",\"username\":\"user_123456\",\"iat\":1700000000,\"exp\":4102444800}"
                        .getBytes(StandardCharsets.UTF_8));
        String signed = hdr + "." + pay;

        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(secret.getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
        String sig = java.util.Base64.getUrlEncoder().withoutPadding()
                .encodeToString(mac.doFinal(signed.getBytes(StandardCharsets.UTF_8)));

        byte[] authBytes = ("Bearer " + signed + "." + sig).getBytes(StandardCharsets.US_ASCII);
        byte[] postJsonBytes = "{\"body\":\"Warmup post content with \\\"quotes\\\" and UTF-8 \\u0041!\"}"
                .getBytes(StandardCharsets.UTF_8);
        byte[] pgRowBytes = "987654|Sample feed post body for C2 warmup|2026-10-22 14:30:00.409+00|user_123456|42"
                .getBytes(StandardCharsets.UTF_8);

        try (Arena warmupArena = Arena.ofConfined()) {
            MemorySegment authSeg     = warmupArena.allocate(authBytes.length + 16L, 8L);
            MemorySegment postReqSeg  = warmupArena.allocate(postJsonBytes.length + 16L, 8L);
            MemorySegment pgRowSeg    = warmupArena.allocate(pgRowBytes.length + 16L, 8L);
            MemorySegment jwtScratch  = warmupArena.allocate(Jwt.SCRATCH_BYTES, 64L);
            MemorySegment unescapeBuf = warmupArena.allocate(4096L, 64L);
            MemorySegment jsonBuf     = warmupArena.allocate(65536L, 64L);
            MemorySegment outBuf      = warmupArena.allocate(65536L, 64L);
            MemorySegment zstream     = warmupArena.allocate(Sys.ISAL_ZSTREAM_ALLOC_SIZE, 64L);

            Sys.isalInitStream(zstream);
            MemorySegment.copy(authBytes, 0, authSeg, JAVA_BYTE, 0L, authBytes.length);
            MemorySegment.copy(postJsonBytes, 0, postReqSeg, JAVA_BYTE, 0L, postJsonBytes.length);
            MemorySegment.copy(pgRowBytes, 0, pgRowSeg, JAVA_BYTE, 0L, pgRowBytes.length);

            long checksum = 0L;
            for (int iter = 0; iter < 20_000; iter++) {
                // 1. Warmup JWT verifyAuthHeader (both valid and invalid paths)
                long uid = Jwt.verifyAuthHeader(authSeg, 0L, authBytes.length, 1700000000L, jwtScratch);
                checksum ^= uid;

                // 2. Warmup POST /posts JSON body parser
                long bodyHandle = Json.parsePostBody(postReqSeg, 0L, postJsonBytes.length, unescapeBuf);
                checksum ^= bodyHandle;

                // 3. Warmup GET /feed 20-row JSON serialization + ISA-L Gzip & Plain framing
                long pos = Json.startFeedObject(jsonBuf, Out.BODY_OFFSET);
                for (int r = 0; r < 20; r++) {
                    pos = Json.writePostFromPgText(
                        jsonBuf, pos, r > 0, pgRowSeg,
                        0L, 6,
                        7L, 35,
                        43L, 26,
                        70L, 11,
                        82L, 2
                    );
                }
                pos = Json.endFeedObject(jsonBuf, pos);
                int jsonLen = (int) (pos - Out.BODY_OFFSET);

                long pPlain = Out.frame200Plain(jsonBuf, jsonLen);
                long pGzip  = Out.frame200Gzip(zstream, jsonBuf, jsonLen, outBuf);
                checksum ^= (pPlain ^ pGzip);

                // 4. Warmup Created Post, Like, and Health serializers
                long cEnd = Json.writeCreatedPostObject(
                    jsonBuf, Out.BODY_OFFSET,
                    pgRowSeg, 0L, 6, 43L, 26,
                    pgRowSeg, 7L, 35, 70L, 11
                );
                checksum ^= Out.frame201Gzip(zstream, jsonBuf, (int) (cEnd - Out.BODY_OFFSET), outBuf);

                long lEnd = Json.writeLikeResponse(jsonBuf, Out.BODY_OFFSET, (iter & 1) == 0, 987654L);
                checksum ^= Out.frame201Plain(jsonBuf, (int) (lEnd - Out.BODY_OFFSET));

                long hEnd = Json.writeHealthResponse(jsonBuf, Out.BODY_OFFSET, true, 120L);
                checksum ^= Out.frame200Plain(jsonBuf, (int) (hEnd - Out.BODY_OFFSET));
            }

            if (checksum == 0xdeadbeefcafeL) {
                System.out.println("Unreachable warmup guard");
            }
        }
    }

    /**
     * Resolves listening ports from PORTS (comma-separated) or PORT + EXTRA_PORT / PORT2.
     * Defaults to [80, 81] if multi-port or [8000] if single-port default.
     */
    private static int[] resolveListenPorts() {
        String portsEnv = System.getenv("PORTS");
        if (portsEnv != null && !portsEnv.isBlank()) {
            String[] parts = portsEnv.split(",");
            int[] out = new int[parts.length];
            int count = 0;
            for (String p : parts) {
                String trimmed = p.trim();
                if (!trimmed.isEmpty()) {
                    out[count++] = Integer.parseInt(trimmed);
                }
            }
            return Arrays.copyOf(out, count);
        }

        String p1 = System.getenv("PORT");
        String p2 = System.getenv("EXTRA_PORT");
        if (p2 == null || p2.isBlank()) {
            p2 = System.getenv("PORT2");
        }

        int port1 = (p1 != null && !p1.isBlank()) ? Integer.parseInt(p1.trim()) : 80;
        if (p2 != null && !p2.isBlank()) {
            int port2 = Integer.parseInt(p2.trim());
            if (port2 != port1) {
                return new int[]{port1, port2};
            }
        } else if (port1 == 80) {
            // In the Million Users Challenge app box, ports 80 and 81 are used simultaneously
            return new int[]{80, 81};
        }
        return new int[]{port1};
    }

    private static String envOrDefault(String key, String fallback) {
        String val = System.getenv(key);
        return (val != null && !val.isBlank()) ? val.trim() : fallback;
    }
}
