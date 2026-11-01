package com.example.feed.health;

import io.smallrye.mutiny.Uni;
import jakarta.ws.rs.GET;
import jakarta.ws.rs.Path;
import jakarta.ws.rs.Produces;
import jakarta.ws.rs.core.MediaType;
import jakarta.ws.rs.core.Response;
import java.lang.management.ManagementFactory;
import io.vertx.mutiny.sqlclient.SqlClient;

@Path("/health")
@Produces(MediaType.APPLICATION_JSON)
public class HealthController {
    public record Health(String status, String db, long uptimeS) {
    }

    public record Degraded(String status, String db, String error) {
    }

    private final SqlClient pool;

    public HealthController(SqlClient pool) {
        this.pool = pool;
    }

    @GET
    public Uni<Response> health() {
        return pool.query("SELECT 1").execute()
                .map(rows -> Response.ok(new Health("ok", "ok",
                        ManagementFactory.getRuntimeMXBean().getUptime() / 1000)).build())
                .onFailure().recoverWithItem(ex -> Response.status(503)
                        .entity(new Degraded("degraded", "unreachable", ex.getMessage())).build());
    }
}
