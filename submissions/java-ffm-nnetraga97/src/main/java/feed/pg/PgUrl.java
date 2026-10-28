package feed.pg;

import java.net.URI;
import java.net.URLDecoder;
import java.nio.charset.StandardCharsets;

/** Connection settings from a {@code postgres://user:pass@host:port/db} URL (the DATABASE_URL format). */
public record PgUrl(String host, int port, String database, String user, String password) {

    public static PgUrl parse(String url) {
        URI uri = URI.create(url);
        if (!"postgres".equals(uri.getScheme()) && !"postgresql".equals(uri.getScheme())) {
            throw new IllegalArgumentException("not a postgres:// URL: " + uri.getScheme());
        }
        String user = null;
        String password = null;
        if (uri.getRawUserInfo() != null) {
            String[] parts = uri.getRawUserInfo().split(":", 2);
            user = decode(parts[0]);
            password = parts.length > 1 ? decode(parts[1]) : null;
        }
        if (user == null || user.isEmpty()) {
            user = System.getProperty("user.name"); // libpq's default: the OS user
        }
        String path = uri.getRawPath();
        String database = path == null || path.length() <= 1 ? user : decode(path.substring(1));
        return new PgUrl(uri.getHost(), uri.getPort() == -1 ? 5432 : uri.getPort(), database, user, password);
    }

    /** Percent-decoding of a URI component: unlike form decoding, '+' stays '+'. */
    private static String decode(String s) {
        return URLDecoder.decode(s.replace("+", "%2B"), StandardCharsets.UTF_8);
    }
}
