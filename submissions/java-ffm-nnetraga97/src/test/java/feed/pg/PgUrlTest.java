package feed.pg;

import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertNull;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

class PgUrlTest {

    @Test
    void parsesTheCiForm() {
        assertEquals(new PgUrl("127.0.0.1", 5432, "feed_test", "postgres", "postgres"),
                PgUrl.parse("postgres://postgres:postgres@127.0.0.1:5432/feed_test"));
    }

    @Test
    void acceptsPostgresqlScheme() {
        assertEquals("db", PgUrl.parse("postgresql://u:p@h:1/db").database());
    }

    @Test
    void keepsAPlusSignInThePassword() {
        assertEquals("a+b", PgUrl.parse("postgres://u:a+b@h:1/db").password());
    }

    @Test
    void decodesPercentEscapedCredentials() {
        PgUrl url = PgUrl.parse("postgres://fe%40ed:p%3Ass%2Fw@db.example:6543/feed");
        assertEquals("fe@ed", url.user());
        assertEquals("p:ss/w", url.password());
        assertEquals(6543, url.port());
    }

    @Test
    void defaultsPortTo5432() {
        assertEquals(5432, PgUrl.parse("postgres://u:p@host/feed").port());
    }

    @Test
    void passwordIsOptional() {
        PgUrl url = PgUrl.parse("postgres://nick@localhost:5432/feed_test");
        assertEquals("nick", url.user());
        assertNull(url.password());
    }

    @Test
    void defaultsUserAndDatabaseToTheOsUser() {
        String os = System.getProperty("user.name");
        PgUrl url = PgUrl.parse("postgres://localhost:5432/feed");
        assertEquals(os, url.user());
        assertEquals("feed", url.database());
        assertEquals(os, PgUrl.parse("postgres://localhost").database());
    }

    @Test
    void ignoresQueryString() {
        assertEquals("feed", PgUrl.parse("postgres://u:p@h:5432/feed?sslmode=disable").database());
    }

    @Test
    void rejectsOtherSchemes() {
        assertThrows(IllegalArgumentException.class, () -> PgUrl.parse("mysql://u:p@h/db"));
    }
}
