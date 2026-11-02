package feed.pg;

import java.nio.charset.StandardCharsets;
import java.security.GeneralSecurityException;
import java.security.MessageDigest;
import java.security.SecureRandom;
import java.util.Base64;

import javax.crypto.Mac;
import javax.crypto.SecretKeyFactory;
import javax.crypto.spec.PBEKeySpec;
import javax.crypto.spec.SecretKeySpec;

/**
 * Client side of SCRAM-SHA-256 (RFC 5802 / 7677) without channel binding ({@code n,,}: the database box runs with
 * ssl off). Runs once per connection at startup, so it allocates freely. The password is used as-is: SASLprep is a
 * no-op for ASCII passwords, which is all this server is given.
 */
public final class Scram {

    private final String username;
    private final String password;
    private final String clientNonce;
    private String clientFirstBare;
    private byte[] expectedServerSignature;

    public Scram(String username, String password, String clientNonce) {
        this.username = username;
        this.password = password;
        this.clientNonce = clientNonce;
    }

    /** 18 random bytes, base64: no commas, as the attribute syntax needs. */
    static String randomNonce() {
        byte[] bytes = new byte[18];
        new SecureRandom().nextBytes(bytes);
        return Base64.getEncoder().encodeToString(bytes);
    }

    public String clientFirstMessage() {
        clientFirstBare = "n=" + username + ",r=" + clientNonce;
        return "n,," + clientFirstBare;
    }

    public String clientFinalMessage(String serverFirstMessage) {
        String nonce = attribute(serverFirstMessage, 'r');
        if (!nonce.startsWith(clientNonce) || nonce.length() == clientNonce.length()) {
            throw new IllegalStateException("SCRAM: server nonce doesn't extend the client nonce");
        }
        byte[] salt = Base64.getDecoder().decode(attribute(serverFirstMessage, 's'));
        int iterations = Integer.parseInt(attribute(serverFirstMessage, 'i'));

        String withoutProof = "c=biws,r=" + nonce;
        byte[] authMessage = utf8(clientFirstBare + "," + serverFirstMessage + "," + withoutProof);
        try {
            byte[] salted = SecretKeyFactory.getInstance("PBKDF2WithHmacSHA256")
                    .generateSecret(new PBEKeySpec(password.toCharArray(), salt, iterations, 256))
                    .getEncoded();
            byte[] clientKey = hmac(salted, utf8("Client Key"));
            byte[] storedKey = MessageDigest.getInstance("SHA-256").digest(clientKey);
            byte[] proof = hmac(storedKey, authMessage);
            for (int i = 0; i < proof.length; i++) {
                proof[i] ^= clientKey[i];
            }
            expectedServerSignature = hmac(hmac(salted, utf8("Server Key")), authMessage);
            return withoutProof + ",p=" + Base64.getEncoder().encodeToString(proof);
        } catch (GeneralSecurityException e) {
            throw new IllegalStateException("SCRAM: " + e.getMessage(), e);
        }
    }

    public void verifyServerFinal(String serverFinalMessage) {
        if (serverFinalMessage.startsWith("e=")) {
            throw new IllegalStateException("SCRAM: server error " + serverFinalMessage.substring(2));
        }
        byte[] signature = Base64.getDecoder().decode(attribute(serverFinalMessage, 'v'));
        if (expectedServerSignature == null || !MessageDigest.isEqual(signature, expectedServerSignature)) {
            throw new IllegalStateException("SCRAM: wrong server signature");
        }
    }

    private static String attribute(String message, char name) {
        for (String part : message.split(",")) {
            if (part.length() >= 2 && part.charAt(0) == name && part.charAt(1) == '=') {
                return part.substring(2);
            }
        }
        throw new IllegalStateException("SCRAM: no '" + name + "' attribute in " + message);
    }

    private static byte[] hmac(byte[] key, byte[] data) throws GeneralSecurityException {
        Mac mac = Mac.getInstance("HmacSHA256");
        mac.init(new SecretKeySpec(key, "HmacSHA256"));
        return mac.doFinal(data);
    }

    private static byte[] utf8(String s) {
        return s.getBytes(StandardCharsets.UTF_8);
    }
}
