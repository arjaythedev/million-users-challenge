package challenge;

import com.fasterxml.jackson.annotation.JsonPropertyOrder;
import io.micronaut.serde.annotation.Serdeable;
import java.util.List;

public final class Responses {
    private Responses() {}

    @Serdeable @JsonPropertyOrder({"id", "body", "created_at", "author", "like_count"})
    public record Post(long id, String body, String created_at, String author, long like_count) {}
    @Serdeable public record Feed(List<Post> posts) {}
    @Serdeable public record Single(Post post) {}
    @Serdeable public record Error(String error) {}
    @Serdeable @JsonPropertyOrder({"liked", "already_liked", "post_id"})
    public record Like(boolean liked, boolean already_liked, long post_id) {}
    @Serdeable @JsonPropertyOrder({"status", "db", "uptime_s"})
    public record Health(String status, String db, long uptime_s) {}
    @Serdeable @JsonPropertyOrder({"status", "db", "error"})
    public record Degraded(String status, String db, String error) {}
}
