package challenge;

import io.vertx.core.DeploymentOptions;
import io.vertx.core.Vertx;
import io.vertx.core.VertxOptions;
import io.vertx.core.impl.transports.TransportInternal;
import io.vertx.core.transport.Transport;

public final class Application {
    public static void main(String[] args) {
        int loops = Math.clamp(Integer.getInteger("mu.event-loops", Runtime.getRuntime().availableProcessors()), 1, 512);
        int total = Math.clamp(Integer.getInteger("mu.db-pool", 64), 1, 512);
        int perLoop = Math.min((total + loops - 1) / loops, 512 / loops);
        long started = System.nanoTime();
        Transport transport = Transport.NIO;
        if (System.getProperty("os.name").startsWith("Linux")) {
            var epoll = new DeferAcceptTransport();
            if (!epoll.isAvailable()) throw new IllegalStateException("Epoll transport unavailable", epoll.unavailabilityCause());
            transport = new TransportInternal("epoll", true, null, epoll);
        }
        var vertx = Vertx.builder().with(new VertxOptions().setEventLoopPoolSize(loops)).withTransport(transport).build();
        System.out.printf("transport=%s loops=%d db_pool_per_loop=%d%n", transport.name(), loops, perLoop);
        vertx.deployVerticle(() -> new FeedVerticle(perLoop, started), new DeploymentOptions().setInstances(loops))
                .onFailure(error -> {
                    error.printStackTrace();
                    vertx.close().onComplete(ignored -> System.exit(1));
                });
    }
}
