package feed.pg;

import static feed.pg.Backend.concat;
import static feed.pg.Backend.cstring;
import static feed.pg.Backend.int4;
import static feed.pg.Backend.int8;
import static feed.pg.Backend.text;
import static org.junit.jupiter.api.Assertions.assertArrayEquals;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertFalse;
import static org.junit.jupiter.api.Assertions.assertThrows;
import static org.junit.jupiter.api.Assertions.assertTrue;

import java.nio.ByteBuffer;
import java.nio.charset.StandardCharsets;
import java.security.MessageDigest;
import java.util.Arrays;
import java.util.Base64;
import java.util.HexFormat;
import java.util.List;
import java.util.Map;

import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

import org.junit.jupiter.api.Test;

class PgProtocolTest {

    private static final List<PgStatement> STATEMENTS = List.of(
            new PgStatement("a", "SELECT $1, $2", PgStatement.INT8, PgStatement.TEXT),
            new PgStatement("b", "SELECT 1"));

    private static PgProtocol protocol(String password) {
        return new PgProtocol(new PgUrl("h", 5432, "feed", "nick", password), STATEMENTS, () -> "fixednonce");
    }

    /** A connection that has finished startup (trust auth) and prepared its statements. */
    private static PgProtocol ready() {
        PgProtocol pg = protocol(null);
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().trustStartup().prepared(STATEMENTS.size()).bytes());
        Frontend.drain(pg);
        assertTrue(pg.isReady());
        return pg;
    }

    private static void feed(PgProtocol pg, byte[] bytes) {
        pg.receive(bytes, 0, bytes.length);
    }

    // ---- startup ----

    @Test
    void startupMessageCarriesProtocol3UserAndDatabase() {
        PgProtocol pg = protocol(null);
        pg.start();
        Map<String, String> startup = Frontend.startup(Frontend.drain(pg));
        assertEquals("196608", startup.get("protocol"));
        assertEquals("nick", startup.get("user"));
        assertEquals("feed", startup.get("database"));
        assertEquals("UTF8", startup.get("client_encoding"));
    }

    @Test
    void trustStartupPreparesEveryStatementThenBecomesReady() {
        PgProtocol pg = protocol(null);
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().trustStartup().bytes());
        assertFalse(pg.isReady());

        List<Frontend.Message> sent = Frontend.messages(Frontend.drain(pg));
        assertEquals("PPS", Frontend.types(sent));
        Frontend.Message parse = sent.get(0);
        assertEquals("a", parse.cstring(0));
        assertEquals("SELECT $1, $2", parse.cstring(2));
        ByteBuffer types = ByteBuffer.wrap(parse.body(), 2 + "SELECT $1, $2".length() + 1, 10);
        assertEquals(2, types.getShort());
        assertEquals(PgStatement.INT8, types.getInt());
        assertEquals(PgStatement.TEXT, types.getInt());

        feed(pg, new Backend().prepared(STATEMENTS.size()).bytes());
        assertTrue(pg.isReady());
        assertFalse(pg.isFailed());
    }

    @Test
    void cleartextPasswordIsSentWhenAsked() {
        PgProtocol pg = protocol("secret");
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().authCleartext().bytes());
        List<Frontend.Message> sent = Frontend.messages(Frontend.drain(pg));
        assertEquals("p", Frontend.types(sent));
        assertEquals("secret", sent.get(0).cstring(0));
    }

    @Test
    void md5PasswordIsHashedWithUserAndSalt() throws Exception {
        PgProtocol pg = protocol("secret");
        pg.start();
        Frontend.drain(pg);
        byte[] salt = {1, 2, 3, 4};
        feed(pg, new Backend().authMd5(salt).bytes());

        MessageDigest md5 = MessageDigest.getInstance("MD5");
        String inner = HexFormat.of().formatHex(md5.digest(text("secretnick")));
        String expected = "md5" + HexFormat.of().formatHex(md5.digest(concat(text(inner), salt)));
        List<Frontend.Message> sent = Frontend.messages(Frontend.drain(pg));
        assertEquals(expected, sent.get(0).cstring(0));
    }

    @Test
    void scramSha256LogsInAndThenPrepares() throws Exception {
        PgProtocol pg = protocol("pencil");
        pg.start();
        Frontend.drain(pg);

        feed(pg, new Backend().authSasl("SCRAM-SHA-256-PLUS", "SCRAM-SHA-256").bytes());
        Frontend.Message initial = Frontend.messages(Frontend.drain(pg)).get(0);
        assertEquals('p', initial.type());
        assertEquals("SCRAM-SHA-256", initial.cstring(0));
        ByteBuffer rest = ByteBuffer.wrap(initial.body(), "SCRAM-SHA-256".length() + 1, initial.body().length
                - "SCRAM-SHA-256".length() - 1);
        byte[] clientFirst = new byte[rest.getInt()];
        rest.get(clientFirst);
        assertEquals("n,,n=,r=fixednonce", new String(clientFirst, StandardCharsets.UTF_8));

        String salt = Base64.getEncoder().encodeToString(new byte[] {9, 8, 7, 6, 5, 4, 3, 2});
        String serverFirst = "r=fixednonceSERVERPART,s=" + salt + ",i=4096";
        feed(pg, new Backend().authSaslContinue(serverFirst).bytes());
        Frontend.Message response = Frontend.messages(Frontend.drain(pg)).get(0);
        String clientFinal = new String(response.body(), StandardCharsets.UTF_8);
        assertTrue(clientFinal.startsWith("c=biws,r=fixednonceSERVERPART,p="), clientFinal);

        String withoutProof = clientFinal.substring(0, clientFinal.indexOf(",p="));
        String serverSignature = serverSignature("pencil", salt, 4096,
                "n=,r=fixednonce," + serverFirst + "," + withoutProof);
        feed(pg, new Backend().authSaslFinal("v=" + serverSignature).trustStartup().bytes());
        assertFalse(pg.isFailed(), pg.failure());
        assertEquals("PPS", Frontend.types(Frontend.messages(Frontend.drain(pg))));
        feed(pg, new Backend().prepared(STATEMENTS.size()).bytes());
        assertTrue(pg.isReady());
    }

    @Test
    void scramFailsOnAWrongServerSignature() {
        PgProtocol pg = protocol("pencil");
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().authSasl("SCRAM-SHA-256").bytes());
        Frontend.drain(pg);
        String salt = Base64.getEncoder().encodeToString(new byte[8]);
        feed(pg, new Backend().authSaslContinue("r=fixednonceX,s=" + salt + ",i=4096").bytes());
        Frontend.drain(pg);
        feed(pg, new Backend().authSaslFinal("v=" + Base64.getEncoder().encodeToString(new byte[32])).bytes());
        assertTrue(pg.isFailed());
        assertFalse(pg.isReady());
    }

    @Test
    void scramFailsWhenTheServerSkipsItsProof() {
        PgProtocol pg = protocol("pencil");
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().authSasl("SCRAM-SHA-256").bytes());
        Frontend.drain(pg);
        String salt = Base64.getEncoder().encodeToString(new byte[8]);
        feed(pg, new Backend().authSaslContinue("r=fixednonceX,s=" + salt + ",i=4096").bytes());
        Frontend.drain(pg);
        feed(pg, new Backend().trustStartup().bytes()); // AuthenticationOk with no SASLFinal before it
        assertTrue(pg.isFailed());
        assertFalse(pg.isReady());
    }

    @Test
    void scramFailsWhenTheServerSaysOkStraightAfterTheMechanismList() {
        PgProtocol pg = protocol("pencil");
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().authSasl("SCRAM-SHA-256").trustStartup().bytes());
        assertTrue(pg.isFailed());
    }

    @Test
    void saslWithoutScramSha256Fails() {
        PgProtocol pg = protocol("pencil");
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().authSasl("SCRAM-SHA-1").bytes());
        assertTrue(pg.isFailed());
    }

    @Test
    void anErrorDuringStartupFailsTheConnection() {
        PgProtocol pg = protocol("wrong");
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().error("28P01", "password authentication failed for user \"nick\"").bytes());
        assertTrue(pg.isFailed());
        assertTrue(pg.failure().contains("password authentication failed"), pg.failure());
    }

    @Test
    void anErrorWhilePreparingFailsTheConnection() {
        PgProtocol pg = protocol(null);
        pg.start();
        Frontend.drain(pg);
        feed(pg, new Backend().trustStartup().parseComplete().error("42P01", "relation does not exist")
                .readyForQuery().bytes());
        assertTrue(pg.isFailed());
        assertFalse(pg.isReady());
    }

    @Test
    void requestsAreRefusedBeforeTheConnectionIsReady() {
        PgProtocol pg = protocol(null);
        pg.start();
        assertThrows(IllegalStateException.class, () -> pg.bind(0));
    }

    // ---- requests ----

    @Test
    void bindSendsBinaryParametersAndAsksForBinaryResults() {
        PgProtocol pg = ready();
        pg.bind(0);
        pg.paramInt8(5);
        byte[] utf8 = text("hé");
        pg.paramText(utf8, 0, utf8.length);
        pg.execute(new EventLog(), 7);
        assertEquals(1, pg.pending());

        List<Frontend.Message> sent = Frontend.messages(Frontend.drain(pg));
        assertEquals("BES", Frontend.types(sent));
        byte[] expectedBind = concat(cstring(""), cstring("a"), shorts(1, 1, 2), int4(8), int8(5), int4(3), utf8,
                shorts(1, 1));
        assertArrayEquals(expectedBind, sent.get(0).body());
        assertArrayEquals(concat(cstring(""), int4(0)), sent.get(1).body());
        assertEquals(0, sent.get(2).body().length);
    }

    @Test
    void aRequestGetsItsRowsThenCompleteThenDone() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        pg.bind(0);
        pg.paramInt8(5);
        pg.paramText(text("hi"), 0, 2);
        pg.execute(log, 1);
        feed(pg, oneRequestReply());
        assertEquals(List.of("row 1 0000000000000005 6869", "row 1 0000000000000006 null", "complete 1 2", "done 1"),
                log.events);
        assertEquals(0, pg.pending());
    }

    @Test
    void messagesSplitAtAnyByteGiveTheSameEvents() {
        byte[] reply = oneRequestReply();
        List<String> whole = runOneRequest(reply, reply.length);
        for (int chunk = 1; chunk < reply.length; chunk++) {
            assertEquals(whole, runOneRequest(reply, chunk), "chunk size " + chunk);
        }
    }

    @Test
    void rowAccessorsDecodeBinaryColumns() {
        PgProtocol pg = ready();
        int[] rows = new int[1];
        pg.bind(1);
        pg.execute(new PgHandler() {
            @Override
            public void onRow(long tag, PgRow row) {
                rows[0]++;
                assertEquals(4, row.columnCount());
                assertEquals(-2L, row.int8(0));
                assertEquals(123456, row.int4(1));
                assertEquals("café", row.text(2));
                assertTrue(row.isNull(3));
                assertFalse(row.isNull(2));
                assertEquals(5, row.length(2));
            }

            @Override
            public void onDone(long tag) {
            }
        }, 1);
        feed(pg, new Backend().bindComplete().dataRow(int8(-2), int4(123456), text("café"), null)
                .commandComplete("SELECT 1").readyForQuery().bytes());
        assertEquals(1, rows[0]);
    }

    @Test
    void anErrorFailsOnlyItsOwnRequest() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        for (int tag = 1; tag <= 3; tag++) {
            pg.bind(1);
            pg.execute(log, tag);
        }
        feed(pg, new Backend()
                .bindComplete().commandComplete("INSERT 0 1").readyForQuery()
                .error("23503", "insert or update on table \"likes\" violates foreign key constraint")
                .readyForQuery()
                .bindComplete().commandComplete("INSERT 0 0").readyForQuery().bytes());
        assertEquals(List.of("complete 1 1", "done 1",
                "error 2 23503 insert or update on table \"likes\" violates foreign key constraint", "done 2",
                "complete 3 0", "done 3"), log.events);
    }

    @Test
    void errorMatchesSqlStateWithoutAllocating() {
        PgProtocol pg = ready();
        boolean[] matched = new boolean[2];
        pg.bind(1);
        pg.execute(new PgHandler() {
            @Override
            public void onError(long tag, PgError error) {
                matched[0] = error.is("23503");
                matched[1] = error.is("23505");
            }

            @Override
            public void onDone(long tag) {
            }
        }, 1);
        feed(pg, new Backend().error("23503", "fk").readyForQuery().bytes());
        assertTrue(matched[0]);
        assertFalse(matched[1]);
    }

    @Test
    void commandTagsGiveRowCounts() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        String[] tags = {"INSERT 0 1", "INSERT 0 0", "SELECT 20", "UPDATE 3"};
        Backend reply = new Backend();
        for (int i = 0; i < tags.length; i++) {
            pg.bind(1);
            pg.execute(log, i);
            reply.bindComplete().commandComplete(tags[i]).readyForQuery();
        }
        feed(pg, reply.bytes());
        assertEquals(List.of("complete 0 1", "done 0", "complete 1 0", "done 1", "complete 2 20", "done 2",
                "complete 3 3", "done 3"), log.events);
    }

    @Test
    void asynchronousMessagesAreIgnored() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        pg.bind(1);
        pg.execute(log, 1);
        feed(pg, new Backend().notice("hello").bindComplete().parameterStatus("TimeZone", "UTC")
                .commandComplete("SELECT 0").notice("again").readyForQuery().bytes());
        assertEquals(List.of("complete 1 0", "done 1"), log.events);
    }

    @Test
    void aRowLargerThanTheReadBufferArrivesIntact() {
        PgProtocol pg = ready();
        byte[] big = new byte[300_000];
        Arrays.fill(big, (byte) 'x');
        int[] length = new int[1];
        pg.bind(1);
        pg.execute(new PgHandler() {
            @Override
            public void onRow(long tag, PgRow row) {
                length[0] = row.length(0);
                assertEquals('x', row.buffer()[row.offset(0) + row.length(0) - 1]);
            }

            @Override
            public void onDone(long tag) {
            }
        }, 1);
        byte[] reply = new Backend().bindComplete().dataRow(big).commandComplete("SELECT 1").readyForQuery().bytes();
        for (int at = 0; at < reply.length; at += 8192) {
            pg.receive(reply, at, Math.min(8192, reply.length - at));
        }
        assertEquals(big.length, length[0]);
    }

    @Test
    void partiallyWrittenOutputKeepsTheRestInOrder() {
        PgProtocol pg = ready();
        pg.bind(1);
        pg.execute(new EventLog(), 1);
        ByteBuffer out = pg.output();
        byte[] first = new byte[3];
        out.get(first);
        pg.bind(1);
        pg.execute(new EventLog(), 2);
        byte[] rest = Frontend.drain(pg);
        assertEquals("BESBES", Frontend.types(Frontend.messages(concat(first, rest))));
        assertFalse(pg.hasOutput());
    }

    @Test
    void readBufferAndProcessHandleBytesReadDirectly() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        pg.bind(1);
        pg.execute(log, 9);
        byte[] reply = new Backend().bindComplete().commandComplete("SELECT 0").readyForQuery().bytes();
        pg.readBuffer().put(reply);
        pg.process();
        assertEquals(List.of("complete 9 0", "done 9"), log.events);
    }

    @Test
    void connectionLostFailsEveryPendingRequest() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        pg.bind(1);
        pg.execute(log, 1);
        pg.bind(1);
        pg.execute(log, 2);
        pg.connectionLost("socket closed");
        assertEquals(List.of("error 1 08006 socket closed", "done 1", "error 2 08006 socket closed", "done 2"),
                log.events);
        assertTrue(pg.isFailed());
        assertEquals(0, pg.pending());
    }

    // ---- group commit ----

    @Test
    void aGroupGoesOutWithOneSync() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        grouped(pg, log, 1);
        grouped(pg, log, 2);
        assertEquals(2, pg.grouped());
        pg.endGroup();
        assertEquals(0, pg.grouped());
        assertEquals(2, pg.pending());
        assertEquals("BEBES", Frontend.types(Frontend.messages(Frontend.drain(pg))));
    }

    @Test
    void endingAnEmptyGroupSendsNothing() {
        PgProtocol pg = ready();
        pg.endGroup();
        assertFalse(pg.hasOutput());
        assertEquals(0, pg.pending());
    }

    @Test
    void aGroupsResultsArriveOnlyWithItsReadyForQuery() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        grouped(pg, log, 1);
        grouped(pg, log, 2);
        pg.endGroup();
        feed(pg, new Backend().bindComplete().dataRow(int8(1), null).commandComplete("SELECT 1")
                .bindComplete().commandComplete("SELECT 0").bytes());
        assertEquals(List.of(), log.events, "results before the group committed");
        feed(pg, new Backend().readyForQuery().bytes());
        assertEquals(List.of("row 1 0000000000000001 null", "complete 1 1", "done 1", "complete 2 0", "done 2"),
                log.events);
        assertEquals(0, pg.pending());
    }

    @Test
    void aGroupsRepliesSplitAtAnyByteGiveTheSameEvents() {
        byte[] reply = new Backend().bindComplete().dataRow(int8(1), null).commandComplete("SELECT 1")
                .notice("hi").bindComplete().commandComplete("SELECT 0").readyForQuery().bytes();
        List<String> whole = runGroupOfTwo(reply, reply.length);
        assertEquals(List.of("row 1 0000000000000001 null", "complete 1 1", "done 1", "complete 2 0", "done 2"),
                whole);
        for (int chunk = 1; chunk < reply.length; chunk++) {
            assertEquals(whole, runGroupOfTwo(reply, chunk), "chunk size " + chunk);
        }
    }

    @Test
    void aFailedGroupIsSentAgainOneRequestPerTransaction() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        for (int tag = 1; tag <= 3; tag++) {
            grouped(pg, log, tag);
        }
        pg.endGroup();
        List<Frontend.Message> first = Frontend.messages(Frontend.drain(pg));
        feed(pg, new Backend().bindComplete().commandComplete("INSERT 0 1")
                .bindComplete().error("23503", "no such post").readyForQuery().bytes());
        assertEquals(List.of(), log.events, "a rolled-back group reports nothing");
        assertEquals(3, pg.pending());

        List<Frontend.Message> again = Frontend.messages(Frontend.drain(pg));
        assertEquals("BESBESBES", Frontend.types(again));
        for (int i = 0; i < 3; i++) {
            assertArrayEquals(first.get(2 * i).body(), again.get(3 * i).body(), "bind " + i);
        }
        feed(pg, new Backend().bindComplete().commandComplete("INSERT 0 1").readyForQuery()
                .bindComplete().error("23503", "no such post").readyForQuery()
                .bindComplete().commandComplete("INSERT 0 1").readyForQuery().bytes());
        assertEquals(List.of("complete 1 1", "done 1", "error 2 23503 no such post", "done 2", "complete 3 1",
                "done 3"), log.events);
        assertEquals(0, pg.pending());
    }

    @Test
    void aRetryResendsARequestThatMovedTheOutputBuffer() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        pg.bind(1);
        pg.execute(log, 1);
        byte[] written = new byte[3];
        pg.output().get(written); // part of it written: the buffer shifts on the next growth
        grouped(pg, log, 2);
        pg.bind(0);
        pg.paramInt8(3);
        byte[] big = new byte[100_000];
        Arrays.fill(big, (byte) 'y');
        pg.paramText(big, 0, big.length);
        pg.executeInGroup(log, 3);
        pg.endGroup();
        List<Frontend.Message> first = Frontend.messages(concat(written, Frontend.drain(pg)));
        assertEquals("BESBEBES", Frontend.types(first));

        feed(pg, new Backend().bindComplete().commandComplete("SELECT 0").readyForQuery()
                .bindComplete().error("23503", "no").readyForQuery().bytes());
        List<Frontend.Message> again = Frontend.messages(Frontend.drain(pg));
        assertEquals("BESBES", Frontend.types(again));
        assertArrayEquals(first.get(3).body(), again.get(0).body());
        assertArrayEquals(first.get(5).body(), again.get(3).body());
    }

    @Test
    void aRetryResendsARequestBoundRightAfterAFullBufferWasSent() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        pg.bind(0);
        pg.paramInt8(1);
        byte[] filler = new byte[64 * 1024 - 49]; // Bind 34 + text, Execute 10, Sync 5: the 64 KB buffer exactly
        pg.paramText(filler, 0, filler.length);
        pg.execute(log, 1);
        assertEquals(64 * 1024, Frontend.drain(pg).length);
        grouped(pg, log, 2); // starts over at the front of the buffer
        grouped(pg, log, 3);
        pg.endGroup();
        List<Frontend.Message> first = Frontend.messages(Frontend.drain(pg));

        feed(pg, new Backend().bindComplete().commandComplete("SELECT 0").readyForQuery()
                .bindComplete().error("23503", "no").readyForQuery().bytes());
        List<Frontend.Message> again = Frontend.messages(Frontend.drain(pg));
        assertEquals("BESBES", Frontend.types(again));
        assertArrayEquals(first.get(0).body(), again.get(0).body());
        assertArrayEquals(first.get(2).body(), again.get(3).body());
    }

    @Test
    void aGroupOfOneFailsLikeAnyRequest() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        grouped(pg, log, 1);
        pg.endGroup();
        assertEquals("BES", Frontend.types(Frontend.messages(Frontend.drain(pg))));
        feed(pg, new Backend().bindComplete().error("23503", "no such post").readyForQuery().bytes());
        assertEquals(List.of("error 1 23503 no such post", "done 1"), log.events);
        assertFalse(pg.hasOutput(), "nothing to retry");
    }

    @Test
    void aFailingGroupEndsTheOpenGroupBeforeItsRetries() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        grouped(pg, log, 1);
        grouped(pg, log, 2);
        pg.endGroup();
        Frontend.drain(pg);
        grouped(pg, log, 3); // a newer group, still open when the first one fails
        feed(pg, new Backend().bindComplete().error("40P01", "deadlock detected").readyForQuery().bytes());
        assertEquals(0, pg.grouped());
        assertEquals("BESBESBES", Frontend.types(Frontend.messages(Frontend.drain(pg))));
    }

    @Test
    void aRequestAfterAGroupGetsItsResultsAfterTheGroups() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        grouped(pg, log, 1);
        grouped(pg, log, 2);
        pg.endGroup();
        pg.bind(1);
        pg.execute(log, 3);
        feed(pg, new Backend().bindComplete().commandComplete("INSERT 0 1").bindComplete()
                .commandComplete("INSERT 0 1").readyForQuery()
                .bindComplete().commandComplete("SELECT 0").readyForQuery().bytes());
        assertEquals(List.of("complete 1 1", "done 1", "complete 2 1", "done 2", "complete 3 0", "done 3"),
                log.events);
    }

    @Test
    void handlersMaySendRequestsWhileALargeGroupIsDelivered() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        PgHandler chaining = new PgHandler() {
            @Override
            public void onComplete(long tag, long rows) {
                log.onComplete(tag, rows);
            }

            @Override
            public void onDone(long tag) {
                log.onDone(tag);
                pg.bind(1);
                pg.execute(log, 1000 + tag);
            }
        };
        int n = 300; // more than the request ring holds at first, so it grows mid-delivery
        Backend reply = new Backend();
        for (int tag = 0; tag < n; tag++) {
            grouped(pg, chaining, tag);
            reply.bindComplete().commandComplete("INSERT 0 1");
        }
        pg.endGroup();
        feed(pg, reply.readyForQuery().bytes());
        assertEquals(n, pg.pending());
        Backend followUps = new Backend();
        for (int i = 0; i < n; i++) {
            followUps.bindComplete().commandComplete("SELECT 0").readyForQuery();
        }
        feed(pg, followUps.bytes());
        assertEquals(4 * n, log.events.size());
        for (int tag = 0; tag < n; tag++) {
            assertEquals("complete " + tag + " 1", log.events.get(2 * tag));
            assertEquals("done " + tag, log.events.get(2 * tag + 1));
            assertEquals("done " + (1000 + tag), log.events.get(2 * n + 2 * tag + 1));
        }
        assertEquals(0, pg.pending());
    }

    @Test
    void connectionLostFailsEveryRequestInAGroup() {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        grouped(pg, log, 1);
        grouped(pg, log, 2);
        pg.endGroup();
        feed(pg, new Backend().bindComplete().commandComplete("INSERT 0 1").bytes());
        pg.connectionLost("socket closed");
        assertEquals(List.of("error 1 08006 socket closed", "done 1", "error 2 08006 socket closed", "done 2"),
                log.events);
        assertEquals(0, pg.pending());
    }

    // ---- helpers ----

    /** Request {@code tag} on statement "a" ($1 = tag), joining the open group. */
    private static void grouped(PgProtocol pg, PgHandler handler, long tag) {
        pg.bind(0);
        pg.paramInt8(tag);
        byte[] text = text("t" + tag);
        pg.paramText(text, 0, text.length);
        pg.executeInGroup(handler, tag);
    }

    private static List<String> runGroupOfTwo(byte[] reply, int chunk) {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        grouped(pg, log, 1);
        grouped(pg, log, 2);
        pg.endGroup();
        Frontend.drain(pg);
        for (int at = 0; at < reply.length; at += chunk) {
            pg.receive(reply, at, Math.min(chunk, reply.length - at));
        }
        return log.events;
    }

    private static byte[] oneRequestReply() {
        return new Backend().bindComplete().dataRow(int8(5), text("hi")).dataRow(int8(6), null)
                .commandComplete("SELECT 2").readyForQuery().bytes();
    }

    private static List<String> runOneRequest(byte[] reply, int chunk) {
        PgProtocol pg = ready();
        EventLog log = new EventLog();
        pg.bind(0);
        pg.paramInt8(5);
        pg.paramText(text("hi"), 0, 2);
        pg.execute(log, 1);
        for (int at = 0; at < reply.length; at += chunk) {
            pg.receive(reply, at, Math.min(chunk, reply.length - at));
        }
        return log.events;
    }

    private static byte[] shorts(int... values) {
        ByteBuffer b = ByteBuffer.allocate(values.length * 2);
        for (int v : values) {
            b.putShort((short) v);
        }
        return b.array();
    }

    /** RFC 5802 ServerSignature, computed independently of Scram. */
    private static String serverSignature(String password, String salt, int iterations, String authMessage)
            throws Exception {
        byte[] salted = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
                .generateSecret(new PBEKeySpec(password.toCharArray(), Base64.getDecoder().decode(salt), iterations,
                        256))
                .getEncoded();
        byte[] serverKey = hmac(salted, text("Server Key"));
        return Base64.getEncoder().encodeToString(hmac(serverKey, text(authMessage)));
    }

    private static byte[] hmac(byte[] key, byte[] data) throws Exception {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(data);
    }
}
