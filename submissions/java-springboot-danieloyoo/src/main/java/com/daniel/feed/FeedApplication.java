package com.daniel.feed;

import io.jsonwebtoken.Claims;
import io.jsonwebtoken.JwtException;
import io.jsonwebtoken.Jwts;
import jakarta.annotation.PostConstruct;
import org.apache.catalina.connector.Connector;
import org.springframework.beans.factory.annotation.Autowired;
import org.springframework.beans.factory.annotation.Value;
import org.springframework.boot.SpringApplication;
import org.springframework.boot.autoconfigure.SpringBootApplication;
import org.springframework.boot.web.embedded.tomcat.TomcatServletWebServerFactory;
import org.springframework.boot.web.server.WebServerFactoryCustomizer;
import org.springframework.context.annotation.Bean;
import org.springframework.core.env.Environment;
import org.springframework.http.HttpHeaders;
import org.springframework.http.HttpStatus;
import org.springframework.http.ResponseEntity;
import org.springframework.http.converter.HttpMessageNotReadableException;
import org.springframework.jdbc.core.JdbcTemplate;
import org.springframework.jdbc.core.RowMapper;
import org.springframework.web.bind.MethodArgumentNotValidException;
import org.springframework.web.bind.annotation.*;
import org.springframework.web.servlet.resource.NoResourceFoundException;
//import org.springframework.core.env.Environment;

import com.fasterxml.jackson.databind.ObjectMapper;
import com.daniel.feed.FeedApplication.PostData;
import com.fasterxml.jackson.databind.JsonNode;

import javax.crypto.SecretKey;
import javax.crypto.spec.SecretKeySpec;

import java.net.URI;
import java.nio.charset.StandardCharsets;
import java.sql.SQLException;
import java.sql.Timestamp;
import java.time.Instant;
import java.time.format.DateTimeFormatter;
import java.util.List;
import java.util.logging.Logger;



import org.apache.catalina.connector.Connector;
import org.springframework.boot.ApplicationArguments;
import org.springframework.boot.ApplicationRunner;
import org.springframework.boot.web.context.WebServerApplicationContext;
import org.springframework.boot.web.embedded.tomcat.TomcatWebServer;
import org.springframework.stereotype.Component;


@SpringBootApplication
public class FeedApplication {

    private static final Logger log = Logger.getLogger(FeedApplication.class.getName());

    public static void main(String[] args) {
        SpringApplication.run(FeedApplication.class, args);
    }

    @Bean
    public WebServerFactoryCustomizer<TomcatServletWebServerFactory> additionalPortCustomizer(org.springframework.core.env.Environment env) {
        return serverFactory -> {
            int port = 80;
            // 3000;
            int extraPort = 81;
            // 3001;
            String host = env.getProperty("HOST", "0.0.0.0");
            try {
                String pEnv = env.getProperty("PORT");
                if (pEnv != null) port = Integer.parseInt(pEnv);
                String epEnv = env.getProperty("EXTRA_PORT");
                if (epEnv != null) extraPort = Integer.parseInt(epEnv);
            } catch (Exception ignored) {}

            // Create a shared Java 21+ Virtual Thread Task Executor
            java.util.concurrent.ExecutorService virtualThreadExecutor =
                java.util.concurrent.Executors.newVirtualThreadPerTaskExecutor();

            // 1. Configure the primary port to run on Virtual Threads
            serverFactory.setPort(port);
            serverFactory.addConnectorCustomizers(connector -> {
                if (connector.getProtocolHandler() instanceof org.apache.coyote.http11.AbstractHttp11Protocol<?> proto) {
                    proto.setMaxConnections(2000000);
                    proto.setAcceptCount(8192);
                    proto.setKeepAliveTimeout(90000);
                    proto.setExecutor(virtualThreadExecutor); // Direct assignment
                }
            });

            // 2. Configure the additional port to use the EXACT same Virtual Thread framework
            Connector additionalConnector = new Connector(TomcatServletWebServerFactory.DEFAULT_PROTOCOL);
            additionalConnector.setPort(extraPort);
            additionalConnector.setProperty("address", host);

            if (additionalConnector.getProtocolHandler() instanceof org.apache.coyote.http11.AbstractHttp11Protocol<?> proto) {
                proto.setMaxConnections(2000000);
                proto.setAcceptCount(8192);
                proto.setKeepAliveTimeout(90000);
                proto.setExecutor(virtualThreadExecutor); // Direct assignment
            }

            serverFactory.addAdditionalTomcatConnectors(additionalConnector);
        };
    }


    @Bean
    public WebServerFactoryCustomizer<org.springframework.boot.web.server.WebServerFactory> dataSourceCustomizer(org.springframework.core.env.Environment env) {
        return serverFactory -> {
            String dbUrl = env.getProperty("DATABASE_URL");
            if (dbUrl != null && dbUrl.startsWith("postgres://")) {
                try {
                    URI uri = new URI(dbUrl);
                    String host = uri.getHost();
                    int port = uri.getPort() == -1 ? 5432 : uri.getPort();
                    String path = uri.getPath();
                    String userInfo = uri.getUserInfo();

                    String jdbcUrl = "jdbc:postgresql://" + host + ":" + port + path;
                    System.setProperty("spring.datasource.url", jdbcUrl);

                    if (userInfo != null) {
                        String[] parts = userInfo.split(":", 2);
                        System.setProperty("spring.datasource.username", parts[0]);
                        if (parts.length > 1) {
                            System.setProperty("spring.datasource.password", parts[1]);
                        }
                    }
                } catch (Exception e) {
                    log.warning("Failed to parse DATABASE_URL: " + e.getMessage());
                }
            }
        };
    }

    // ==================== MODELS & PROJECTIONS ====================

    public static class PostData {
        private Long id;
        private String body;
        private Instant createdAt;
        private String author;
        private int likeCount;

        public PostData() {}

        public PostData(Long id, String body, Instant createdAt, String author, int likeCount) {
            this.id = id;
            this.body = body;
            this.createdAt = createdAt;
            this.author = author;
            this.likeCount = likeCount;
        }

        public Long getId() { return id; }
        public void setId(Long id) { this.id = id; }
        public String getBody() { return body; }
        public void setBody(String body) { this.body = body; }
        public Instant getCreatedAt() { return createdAt; }
        public void setCreatedAt(Instant createdAt) { this.createdAt = createdAt; }
        public String getAuthor() { return author; }
        public void setAuthor(String author) { this.author = author; }
        public int getLikeCount() { return likeCount; }
        public void setLikeCount(int likeCount) { this.likeCount = likeCount; }
    }

    private static final RowMapper<PostData> POST_ROW_MAPPER = (rs, rowNum) -> {
        Timestamp ts = rs.getTimestamp("created_at");
        Instant instant = ts != null ? ts.toInstant() : Instant.now();
        return new PostData(
            rs.getLong("id"),
            rs.getString("body"),
            instant,
            rs.getString("username"),
            rs.getInt("like_count")
        );
    };

    // ==================== DTOS & RESPONSES ====================

    public record HealthResponse(String status, String db, int uptime_s) {}
    public record DegradedHealthResponse(String status, String db, String error) {}
    public record FeedResponse(List<PostDto> posts) {}
    public record PostDto(Long id, String body, String created_at, String author, int like_count) {}
    public record PostResponse(PostDto post) {}
    public record LikeResponse(boolean liked, boolean already_liked, long post_id) {}
    public record ErrorResponse(String error) {}

    // ==================== EXCEPTIONS ====================

    public static class FeedException extends RuntimeException {
        private final HttpStatus httpStatus;
        public FeedException(String message, HttpStatus httpStatus) {
            super(message);
            this.httpStatus = httpStatus;
        }
        public HttpStatus getHttpStatus() { return httpStatus; }
    }

    // ==================== EXCEPTION HANDLER ====================

    @ControllerAdvice
    public static class GlobalExceptionHandler {

        @ExceptionHandler(FeedException.class)
        public ResponseEntity<ErrorResponse> handleFeedException(FeedException e) {
            return ResponseEntity.status(e.getHttpStatus()).body(new ErrorResponse(e.getMessage()));
        }

        @ExceptionHandler(NoResourceFoundException.class)
        public ResponseEntity<ErrorResponse> handleNoResourceFound(NoResourceFoundException e) {
            return ResponseEntity.status(HttpStatus.NOT_FOUND).body(new ErrorResponse("not found"));
        }

        @ExceptionHandler({MethodArgumentNotValidException.class, HttpMessageNotReadableException.class})
        public ResponseEntity<ErrorResponse> handleValidationExceptions(Exception e) {
            return ResponseEntity.status(HttpStatus.BAD_REQUEST).body(new ErrorResponse("malformed JSON body"));
        }

        @ExceptionHandler(Exception.class)
        public ResponseEntity<ErrorResponse> handleGeneralException(Exception e) {
            log.warning("Internal server error: " + e.getMessage());
            return ResponseEntity.status(HttpStatus.INTERNAL_SERVER_ERROR).body(new ErrorResponse("internal server error"));
        }
    }

    // ==================== CONTROLLER ====================

    @RestController
    public static class FeedController {

        private final JdbcTemplate jdbcTemplate;
        private final ObjectMapper objectMapper;
        private Instant serverStartTime;

        @Value("${jwt.secret:million-users-challenge}")
        private String jwtSecret;

        private SecretKey key;

        @PostConstruct
        public void init() {
            this.serverStartTime = Instant.now();
            byte[] keyBytes = jwtSecret.getBytes(StandardCharsets.UTF_8);
            if (keyBytes.length < 32) {
                byte[] padded = new byte[32];
                System.arraycopy(keyBytes, 0, padded, 0, keyBytes.length);
                keyBytes = padded;
            }
            this.key = new SecretKeySpec(keyBytes, "HmacSHA256");
        }

        @Autowired
        public FeedController(JdbcTemplate jdbcTemplate, ObjectMapper objectMapper) {
            this.jdbcTemplate = jdbcTemplate;
            this.objectMapper = objectMapper;
        }

        record AuthenticatedUser(long userId, String username) {}

        // ==================== ENDPOINTS ====================

        @GetMapping("/health")
        public ResponseEntity<?> health() {
            try {
                jdbcTemplate.queryForObject("SELECT 1", Integer.class);
                int uptime = (int) java.time.Duration.between(serverStartTime, Instant.now()).getSeconds();
                return ResponseEntity.ok(new HealthResponse("ok", "ok", uptime));
            } catch (Exception e) {
                return ResponseEntity.status(HttpStatus.SERVICE_UNAVAILABLE)
                        .body(new DegradedHealthResponse("degraded", "unreachable", e.getMessage()));
            }
        }

        @GetMapping("/feed")
        public ResponseEntity<FeedResponse> getFeed() {
            String sql = "SELECT p.id, p.body, p.created_at, u.username, " +
                         "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " +
                         "FROM posts p JOIN users u ON u.id = p.user_id " +
                         "ORDER BY p.created_at DESC, p.id DESC LIMIT 20";

            List<PostData> projections = jdbcTemplate.query(sql, POST_ROW_MAPPER);
            List<PostDto> posts = projections.stream().map(p -> new PostDto(
                    p.getId(),
                    p.getBody(),
                    DateTimeFormatter.ISO_INSTANT.format(p.getCreatedAt().truncatedTo(java.time.temporal.ChronoUnit.MILLIS)),
                    p.getAuthor(),
                    p.getLikeCount()
            )).toList();
            return ResponseEntity.ok(new FeedResponse(posts));
        }

        @GetMapping("/posts/{postId}")
        public ResponseEntity<PostResponse> findPostById(@PathVariable String postId) {
            long id = IdCheck(postId);
            PostData p = PostCheck(id);

            PostDto dto = new PostDto(
                    p.getId(),
                    p.getBody(),
                    DateTimeFormatter.ISO_INSTANT.format(p.getCreatedAt().truncatedTo(java.time.temporal.ChronoUnit.MILLIS)),
                    p.getAuthor(),
                    p.getLikeCount()
            );
            return ResponseEntity.ok(new PostResponse(dto));
        }

        @PostMapping("/posts")
        public ResponseEntity<PostResponse> post(
                @RequestHeader(value = HttpHeaders.AUTHORIZATION, required = false) String authHeader,
                @RequestBody(required = false) String rawBody) {

            AuthenticatedUser user = AuthCheck(authHeader);

            if (rawBody == null || rawBody.trim().isEmpty()) {
                throw new FeedException("body is required", HttpStatus.BAD_REQUEST);
            }

            JsonNode jsonNode;
            try {
                jsonNode = objectMapper.readTree(rawBody);
            } catch (Exception e) {
                throw new FeedException("malformed JSON body", HttpStatus.BAD_REQUEST);
            }

            if (jsonNode == null || !jsonNode.has("body") || jsonNode.get("body").isNull()) {
                throw new FeedException("body is required", HttpStatus.BAD_REQUEST);
            }

            JsonNode bodyVal = jsonNode.get("body");
            if (!bodyVal.isTextual()) {
                throw new FeedException("body is required", HttpStatus.BAD_REQUEST);
            }

            String bodyStr = bodyVal.asText();
            String trimmedBody = bodyStr.trim();
            if (trimmedBody.isEmpty()) {
                throw new FeedException("body is required", HttpStatus.BAD_REQUEST);
            }
            if (trimmedBody.length() > 500) {
                throw new FeedException("body must be at most 500 characters", HttpStatus.BAD_REQUEST);
            }

            String sql = "INSERT INTO posts (user_id, body) VALUES (?, ?) RETURNING id, created_at";
            var result = jdbcTemplate.query(sql, rs -> {
                if (rs.next()) {
                    long newId = rs.getLong("id");
                    Timestamp ts = rs.getTimestamp("created_at");
                    Instant instant = ts != null ? ts.toInstant() : Instant.now();
                    return new Object[]{newId, instant};
                }
                return null;
            }, user.userId(), trimmedBody);

            if (result == null) {
                throw new FeedException("internal server error", HttpStatus.INTERNAL_SERVER_ERROR);
            }

            long postId = (long) result[0];
            Instant createdAt = (Instant) result[1];

            PostDto dto = new PostDto(
                    postId,
                    trimmedBody,
                    DateTimeFormatter.ISO_INSTANT.format(createdAt.truncatedTo(java.time.temporal.ChronoUnit.MILLIS)),
                    user.username(),
                    0
            );

            return ResponseEntity.status(HttpStatus.CREATED).body(new PostResponse(dto));
        }

        @PostMapping("/posts/{postId}/like")
        public ResponseEntity<LikeResponse> like(
                @RequestHeader(value = HttpHeaders.AUTHORIZATION, required = false) String authHeader,
                @PathVariable String postId) {

            AuthenticatedUser user = AuthCheck(authHeader);
            long id = IdCheck(postId);
            int inserted;
            try {
                inserted = jdbcTemplate.update(
                        "INSERT INTO likes (user_id, post_id) VALUES (?, ?) ON CONFLICT (user_id, post_id) DO NOTHING",
                        user.userId(), id
                );
            } catch (org.springframework.dao.DataIntegrityViolationException e) {
                Throwable cause = e;
                while (cause != null) {
                    if (cause instanceof SQLException sqlException
                            && "23503".equals(sqlException.getSQLState())) {
                        throw new FeedException("post not found", HttpStatus.NOT_FOUND);
                    }
                    cause = cause.getCause();
                }
                throw e;
            }

            boolean alreadyLiked = inserted == 0;
            HttpStatus status = alreadyLiked ? HttpStatus.OK : HttpStatus.CREATED;
            return ResponseEntity.status(status).body(new LikeResponse(true, alreadyLiked, id));
        }

        // ==================== CHECK & GUARD METHODS ====================

        public long IdCheck(String idStr) throws FeedException {
            try {
                long id = Long.parseLong(idStr);
                if (id <= 0) {
                    throw new FeedException("invalid post id", HttpStatus.BAD_REQUEST);
                }
                return id;
            } catch (NumberFormatException e) {
                throw new FeedException("invalid post id", HttpStatus.BAD_REQUEST);
            }
        }

        public PostData PostCheck(long id) throws FeedException {
            String sql = "SELECT p.id, p.body, p.created_at, u.username, " +
                         "(SELECT count(*) FROM likes l WHERE l.post_id = p.id)::int AS like_count " +
                         "FROM posts p JOIN users u ON u.id = p.user_id WHERE p.id = ?";

            List<PostData> results = jdbcTemplate.query(sql, POST_ROW_MAPPER, id);
            if (results.isEmpty()) {
                throw new FeedException("post not found", HttpStatus.NOT_FOUND);
            }
            return results.get(0);
        }

        public AuthenticatedUser AuthCheck(String authHeader) throws FeedException {
            if (authHeader == null || !authHeader.startsWith("Bearer ")) {
                throw new FeedException("missing bearer token", HttpStatus.UNAUTHORIZED);
            }
            String token = authHeader.substring(7);
            try {
                Claims claims = Jwts.parser()
                        .verifyWith(key)
                        .build()
                        .parseSignedClaims(token)
                        .getPayload();

                if (claims.getExpiration() == null) {
                    throw new FeedException("invalid or expired token", HttpStatus.UNAUTHORIZED);
                }

                String subStr = claims.getSubject();
                Object usernameObj = claims.get("username");
                String username = usernameObj instanceof String ? (String) usernameObj : null;

                if (subStr == null || username == null) {
                    throw new FeedException("invalid token payload", HttpStatus.UNAUTHORIZED);
                }

                long userId;
                try {
                    userId = Long.parseLong(subStr);
                    if (userId <= 0) {
                        throw new NumberFormatException();
                    }
                } catch (NumberFormatException e) {
                    throw new FeedException("invalid token payload", HttpStatus.UNAUTHORIZED);
                }

                return new AuthenticatedUser(userId, username);
            } catch (FeedException e) {
                throw e;
            } catch (JwtException | IllegalArgumentException e) {
                throw new FeedException("invalid or expired token", HttpStatus.UNAUTHORIZED);

            }
        }
    }


    //used to print tomcat configs for debugging
@Component
public static class PrintConnectors implements ApplicationRunner {

    private final WebServerApplicationContext context;

    // Inject the web server context to access the running server instance
    public PrintConnectors(WebServerApplicationContext context) {
        this.context = context;
    }

    @Override
    public void run(ApplicationArguments args) throws Exception {
        // Retrieve the live Tomcat server instance after startup
        if (context.getWebServer() instanceof TomcatWebServer tomcatWebServer) {

            // Fetch Tomcat's active connectors
            Connector[] connectors = tomcatWebServer.getTomcat().getService().findConnectors();

            for (Connector connector : connectors) {
                System.out.println(
                    "======CONNECTOR PORT ==== " + connector.getLocalPort() // Gets the actual bound runtime port
                    + " Max connections = " + connector.getProperty("maxConnections")
                    + " Accept count = " + connector.getProperty("acceptCount")
                    + " maxThreads = " + connector.getProperty("maxThreads")
                    + " keepAliveTimeout = " + connector.getProperty("keepAliveTimeout")
                );
            }
        }
    }
}

}