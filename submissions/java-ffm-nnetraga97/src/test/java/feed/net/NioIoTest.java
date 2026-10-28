package feed.net;

import java.io.IOException;

class NioIoTest extends IoContract {

    @Override
    Io open() throws IOException {
        return new NioIo();
    }
}
