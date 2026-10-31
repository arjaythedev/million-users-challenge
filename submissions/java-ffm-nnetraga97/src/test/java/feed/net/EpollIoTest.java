package feed.net;

import static org.junit.jupiter.api.Assertions.assertTrue;

import java.io.IOException;
import java.net.Socket;

import org.junit.jupiter.api.Test;
import org.junit.jupiter.api.condition.EnabledOnOs;
import org.junit.jupiter.api.condition.OS;

@EnabledOnOs(OS.LINUX)
class EpollIoTest extends IoContract {

    @Override
    Io open() throws IOException {
        return EpollIo.open();
    }

    @Test
    void acceptsAClientOnlyOnceItHasSentSomething() throws IOException {
        int port = io.listen("127.0.0.1", 0);
        try (Socket client = new Socket("127.0.0.1", port)) {
            io.poll(300, recorder);
            assertTrue(accepted.isEmpty(), "accepted before the client sent anything");
            client.getOutputStream().write('x');
            pollUntil(() -> !accepted.isEmpty());
        }
    }
}
