package feed.net;

import java.io.IOException;

import org.junit.jupiter.api.condition.EnabledOnOs;
import org.junit.jupiter.api.condition.OS;

@EnabledOnOs(OS.LINUX)
class EpollIoTest extends IoContract {

    @Override
    Io open() throws IOException {
        return EpollIo.open();
    }
}
