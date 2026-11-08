package feed.testing;

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.io.InputStream;
import java.net.Socket;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;

/** A minimal blocking HTTP/1.1 client for tests over real sockets. */
public final class HttpClient {

    public record Response(int status, String head, String body, byte[] bytes) {
    }

    private HttpClient() {
    }

    public static String get(String path) {
        return "GET " + path + " HTTP/1.1\r\nHost: t\r\n\r\n";
    }

    /** Opens a connection with a 10 s read timeout. */
    public static Socket connect(int port) throws IOException {
        Socket s = new Socket("127.0.0.1", port);
        s.setSoTimeout(10_000);
        return s;
    }

    public static Response roundTrip(Socket s, String request) throws IOException {
        s.getOutputStream().write(request.getBytes(StandardCharsets.UTF_8));
        return read(s.getInputStream());
    }

    /** Reads one response: the head, then Content-Length bytes of body. */
    public static Response read(InputStream in) throws IOException {
        ByteArrayOutputStream head = new ByteArrayOutputStream();
        while (!head.toString(StandardCharsets.US_ASCII).endsWith("\r\n\r\n")) {
            int c = in.read();
            if (c < 0) {
                throw new IOException("closed after " + head);
            }
            head.write(c);
        }
        String h = head.toString(StandardCharsets.US_ASCII);
        int status = Integer.parseInt(h.substring(9, 12));
        int length = 0;
        for (String line : h.split("\r\n")) {
            if (line.toLowerCase().startsWith("content-length:")) {
                length = Integer.parseInt(line.substring(15).trim());
            }
        }
        byte[] body = in.readNBytes(length);
        return new Response(status, h, new String(body, StandardCharsets.UTF_8), body);
    }

    /** A response body from the challenge's test/golden directory. */
    public static String golden(String file) throws IOException {
        return Files.readString(Path.of("../../test/golden", file)).strip();
    }
}
