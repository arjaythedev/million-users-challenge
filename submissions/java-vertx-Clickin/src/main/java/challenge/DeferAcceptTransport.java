package challenge;

import io.netty.bootstrap.ServerBootstrap;
import io.netty.channel.ChannelOption;
import io.netty.channel.epoll.EpollChannelOption;
import io.vertx.core.impl.transports.EpollTransport;
import io.vertx.core.net.TcpConfig;

// Defer accepts and avoid resetting default socket options on accepted connections.
final class DeferAcceptTransport extends EpollTransport {
    @Override public void configure(TcpConfig config, boolean domainSocket, ServerBootstrap bootstrap) {
        super.configure(config, domainSocket, bootstrap);
        if (!domainSocket) {
            bootstrap.option(EpollChannelOption.TCP_DEFER_ACCEPT, 1);
            bootstrap.childOption(ChannelOption.TCP_NODELAY, null);
            bootstrap.childOption(ChannelOption.SO_KEEPALIVE, null);
        }
    }
}
