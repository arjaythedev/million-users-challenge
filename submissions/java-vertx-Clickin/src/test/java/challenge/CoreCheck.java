package challenge;

import io.netty.bootstrap.Bootstrap;
import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.Channel;
import io.netty.channel.ChannelHandlerContext;
import io.netty.channel.ChannelInboundHandlerAdapter;
import io.netty.channel.ChannelOption;
import io.netty.channel.MultiThreadIoEventLoopGroup;
import io.netty.channel.epoll.EpollChannelOption;
import io.netty.channel.epoll.EpollIoHandler;
import io.netty.channel.epoll.EpollServerSocketChannel;
import io.netty.channel.unix.UnixChannelOption;
import io.vertx.core.MultiMap;
import io.vertx.core.buffer.Buffer;
import io.vertx.core.net.TcpConfig;
import io.vertx.core.net.TcpOption;
import io.vertx.sqlclient.Row;
import java.io.ByteArrayInputStream;
import java.lang.reflect.Proxy;
import java.net.InetSocketAddress;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.time.OffsetDateTime;
import java.time.ZoneOffset;
import java.util.Arrays;
import java.util.Base64;
import java.util.Random;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.TimeUnit;
import java.util.zip.GZIPInputStream;
import javax.crypto.Mac;
import javax.crypto.spec.SecretKeySpec;

public final class CoreCheck {
    public static void main(String[] args) throws Exception {
        gzip();
        validation();
        writer();
        jwt();
        transportOptions();
        if (System.getProperty("os.name").startsWith("Linux")) epoll();
        System.out.println("Gzip, JSON, and JWT checks passed");
    }

    private static void transportOptions() {
        var transport = new DeferAcceptTransport();
        var config = new TcpConfig().setSoReusePort(true).setSoLinger(2)
                .setOption(TcpOption.NODELAY, true).setOption(TcpOption.QUICKACK, true);
        var server = new ServerBootstrap();
        transport.configure(config, false, server);
        var children = server.config().childOptions();
        if (children.containsKey(ChannelOption.TCP_NODELAY) || children.containsKey(ChannelOption.SO_KEEPALIVE))
            throw new AssertionError("redundant accepted socket options");
        if (!Integer.valueOf(2).equals(children.get(ChannelOption.SO_LINGER))
                || !Boolean.TRUE.equals(children.get(EpollChannelOption.TCP_QUICKACK)))
            throw new AssertionError("other accepted socket options");
        if (!Boolean.TRUE.equals(server.config().options().get(UnixChannelOption.SO_REUSEPORT))
                || !Integer.valueOf(1).equals(server.config().options().get(EpollChannelOption.TCP_DEFER_ACCEPT)))
            throw new AssertionError("server socket options");

        var domain = new ServerBootstrap().childOption(ChannelOption.TCP_NODELAY, false)
                .childOption(ChannelOption.SO_KEEPALIVE, true);
        transport.configure(config, true, domain);
        if (!Boolean.FALSE.equals(domain.config().childOptions().get(ChannelOption.TCP_NODELAY))
                || !Boolean.TRUE.equals(domain.config().childOptions().get(ChannelOption.SO_KEEPALIVE))
                || domain.config().options().containsKey(EpollChannelOption.TCP_DEFER_ACCEPT))
            throw new AssertionError("domain socket options");

        var client = new Bootstrap();
        transport.configure(config, false, client);
        if (!Boolean.TRUE.equals(client.config().options().get(ChannelOption.TCP_NODELAY))
                || !Boolean.FALSE.equals(client.config().options().get(ChannelOption.SO_KEEPALIVE)))
            throw new AssertionError("client socket options");
        System.out.println("Transport option checks passed");
    }

    private static void epoll() throws Exception {
        var transport = new DeferAcceptTransport();
        if (!transport.isAvailable()) throw new AssertionError("Epoll unavailable", transport.unavailabilityCause());
        var group = new MultiThreadIoEventLoopGroup(1, EpollIoHandler.newFactory());
        try {
            var accepted = new CompletableFuture<Channel>();
            var bootstrap = new ServerBootstrap().group(group).channel(EpollServerSocketChannel.class)
                    .childHandler(new ChannelInboundHandlerAdapter() {
                        @Override public void channelActive(ChannelHandlerContext ctx) {
                            accepted.complete(ctx.channel());
                            ctx.fireChannelActive();
                        }
                    });
            transport.configure(new TcpConfig(), false, bootstrap);
            var channel = bootstrap.bind("127.0.0.1", 0).sync().channel();
            try {
                if (!Integer.valueOf(1).equals(channel.config().getOption(EpollChannelOption.TCP_DEFER_ACCEPT)))
                    throw new AssertionError("TCP_DEFER_ACCEPT not set");
                try (var client = new Socket("127.0.0.1", ((InetSocketAddress) channel.localAddress()).getPort())) {
                    client.getOutputStream().write(1);
                    var child = accepted.get(5, TimeUnit.SECONDS);
                    try {
                        if (!Boolean.TRUE.equals(child.config().getOption(ChannelOption.TCP_NODELAY))
                                || !Boolean.FALSE.equals(child.config().getOption(ChannelOption.SO_KEEPALIVE)))
                            throw new AssertionError("accepted socket defaults");
                    } finally {
                        child.close().sync();
                    }
                }
            } finally {
                channel.close().sync();
            }
        } finally {
            group.shutdownGracefully(0, 0, TimeUnit.SECONDS).sync();
        }
        System.out.println("Epoll deferred accept and socket checks passed");
    }

    private static void gzip() throws Exception {
        var headers = MultiMap.caseInsensitiveMultiMap();
        for (String value : new String[] {"gzip", "br, GZip;q=0.5", "gzip;q=1"}) {
            headers.set("Accept-Encoding", value);
            if (!Gzip.accepts(headers)) throw new AssertionError(value);
        }
        for (String value : new String[] {"br", "notgzip", "gzip;q=0.000", "gzip;q=2", "gzip;q=bad"}) {
            headers.set("Accept-Encoding", value);
            if (Gzip.accepts(headers)) throw new AssertionError(value);
        }
        headers.set("Accept-Encoding", "br").add("Accept-Encoding", "gzip");
        if (!Gzip.accepts(headers)) throw new AssertionError("multiple headers");
        try (var gzip = new Gzip(1)) {
            var random = new Random(123);
            for (int size : new int[] {0, 1, 1023, 1024, 8192, 32768, 1024}) {
                byte[] data = new byte[size + 8];
                random.nextBytes(data);
                byte[] encoded = gzip.compress(data, 4, size).getBytes();
                try (var decoded = new GZIPInputStream(new ByteArrayInputStream(encoded))) {
                    if (!Arrays.equals(Arrays.copyOfRange(data, 4, size + 4), decoded.readAllBytes())) throw new AssertionError("gzip size " + size);
                }
            }
        }
    }

    private static void validation() {
        if (!Json.body(Buffer.buffer("{\"body\":\"  한😀\\n  \"}")).equals("한😀"))
            throw new AssertionError("JSON string round trip");
        if (!Json.body(Buffer.buffer("{\"body\":\"" + "😀".repeat(500) + "\"}")).equals("😀".repeat(500)))
            throw new AssertionError("character count");
        expect(400, "body must be at most 500 characters", () -> Json.body(Buffer.buffer("{\"body\":\"" + "😀".repeat(501) + "\"}")));
        expect(400, "malformed JSON body", () -> Json.body(Buffer.buffer("{bad")));
        expect(400, "malformed JSON body", () -> Json.body(Buffer.buffer("{\"body\":\"x\"} trailing")));
        expect(400, "body is required", () -> Json.body(Buffer.buffer("{\"body\":42}")));
        expect(400, "body is required", () -> Json.body(Buffer.buffer("{\"body\":\"  \"}")));
        expect(400, "body is required", () -> Json.body(Buffer.buffer("[]")));
    }

    private static void writer() {
        var json = new Json();
        var date = OffsetDateTime.of(1000, 1, 1, 0, 0, 0, 999_999_999, ZoneOffset.ofHours(1));
        Row row = row(Long.MIN_VALUE, date, 2);
        json.single(row, "한😀\"\\\u0000\b\t\n\f\r\u001b\ud800", "a\udc00", Integer.MIN_VALUE, 2);
        String expected = "{\"post\":{\"id\":-9223372036854775808,\"body\":\"한😀\\\"\\\\\\u0000\\b\\t\\n\\f\\r\\u001B\\uD800\",\"created_at\":\"0999-12-31T23:00:00.999Z\",\"author\":\"a\\uDC00\",\"like_count\":-2147483648}}";
        Buffer first = json.buffer();
        if (!first.toString().equals(expected)) throw new AssertionError(first);
        json.single(row(1, OffsetDateTime.MAX, 2), "x".repeat(20_000), "a", 0, 2);
        if (!json.buffer().toString().contains("+1000000000-01-01T17:59:59.999Z")) throw new AssertionError("extended UTC year");
        json.single(row(1, OffsetDateTime.MIN, 1), "x", "a", 0, 1);
        if (!json.buffer().toString().contains("-1000000000-12-31T06:00:00.000Z")) throw new AssertionError("negative UTC year");
        if (!first.toString().equals(expected)) throw new AssertionError("scratch reused before response copy");
        json.like(Long.MAX_VALUE, true);
        if (!json.buffer().toString().equals("{\"liked\":true,\"already_liked\":true,\"post_id\":9223372036854775807}")) throw new AssertionError("like JSON");
        json.health(false, 0);
        if (!json.buffer().toString().equals("{\"status\":\"degraded\",\"db\":\"unreachable\",\"error\":\"internal server error\"}")) throw new AssertionError("health JSON");
    }

    private static Row row(long id, OffsetDateTime date, int dateColumn) {
        return (Row) Proxy.newProxyInstance(Row.class.getClassLoader(), new Class<?>[] {Row.class}, (proxy, method, args) -> switch (method.getName()) {
            case "getLong" -> { if (!args[0].equals(0)) throw new AssertionError("id column"); yield id; }
            case "getOffsetDateTime" -> { if (!args[0].equals(dateColumn)) throw new AssertionError("created_at column"); yield date; }
            default -> throw new UnsupportedOperationException(method.getName());
        });
    }

    private static void jwt() throws Exception {
        var verifier = new JwtVerifier("secret");
        String future = Long.toString(System.currentTimeMillis() / 1000 + 600);
        String good = token("{\"alg\":\"HS256\"}", "{\"sub\":\"1\",\"username\":\"한\",\"exp\":" + future + "}");
        var user = verifier.verify("Bearer " + good);
        if (user.id() != 1 || !user.username().equals("한")) throw new AssertionError("valid token");
        if (verifier.verify("Bearer " + good + "=").id() != 1) throw new AssertionError("padded signature");
        expect(401, "missing bearer token", () -> verifier.verify("Basic " + good));
        expect(401, "invalid or expired token", () -> verifier.verify("Bearer " + good + "x"));
        int signature = good.lastIndexOf('.') + 1;
        String forged = good.substring(0, signature) + (good.charAt(signature) == 'A' ? 'B' : 'A') + good.substring(signature + 1);
        expect(401, "invalid or expired token", () -> verifier.verify("Bearer " + forged));
        if (verifier.verify("Bearer " + good).id() != 1) throw new AssertionError("verifier after invalid signature");
        expect(401, "invalid or expired token", () -> verifier.verify("Bearer " + token("{\"alg\":\"none\"}", "{\"exp\":" + future + "}")));
        expect(401, "invalid or expired token", () -> verifier.verify("Bearer " + token("{\"alg\":\"HS256\"}", "{\"sub\":\"1\",\"username\":\"x\",\"exp\":1}")));
        expect(401, "invalid token payload", () -> verifier.verify("Bearer " + token("{\"alg\":\"HS256\"}", "{\"sub\":1,\"username\":\"x\",\"exp\":" + future + "}")));
        expect(401, "invalid token payload", () -> verifier.verify("Bearer " + token("{\"alg\":\"HS256\"}", "{\"sub\":\"0\",\"username\":\"x\",\"exp\":" + future + "}")));
        expect(401, "invalid token payload", () -> verifier.verify("Bearer " + token("{\"alg\":\"HS256\"}", "{\"sub\":\"1\",\"exp\":" + future + "}")));
    }

    private static String token(String header, String payload) {
        try {
            var encoder = Base64.getUrlEncoder().withoutPadding();
            String data = encoder.encodeToString(header.getBytes(StandardCharsets.UTF_8)) + "." + encoder.encodeToString(payload.getBytes(StandardCharsets.UTF_8));
            Mac mac = Mac.getInstance("HmacSHA256");
            mac.init(new SecretKeySpec("secret".getBytes(StandardCharsets.UTF_8), "HmacSHA256"));
            return data + "." + encoder.encodeToString(mac.doFinal(data.getBytes(StandardCharsets.US_ASCII)));
        } catch (Exception e) {
            throw new AssertionError(e);
        }
    }

    private static void expect(int status, String message, Runnable action) {
        try {
            action.run();
            throw new AssertionError("expected " + message);
        } catch (ApiException error) {
            if (error.status != status || !error.getMessage().equals(message)) throw new AssertionError(error);
        }
    }
}
