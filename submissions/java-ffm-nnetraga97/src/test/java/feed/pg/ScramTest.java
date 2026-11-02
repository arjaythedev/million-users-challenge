package feed.pg;

import static org.junit.jupiter.api.Assertions.assertDoesNotThrow;
import static org.junit.jupiter.api.Assertions.assertEquals;
import static org.junit.jupiter.api.Assertions.assertThrows;

import org.junit.jupiter.api.Test;

/** The example exchange from RFC 7677 section 3 (user "user", password "pencil"). */
class ScramTest {

    private static final String NONCE = "rOprNGfwEbeRWgbNEkqO";
    private static final String SERVER_FIRST =
            "r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096";

    @Test
    void clientFirstMessageMatchesRfc() {
        assertEquals("n,,n=user,r=rOprNGfwEbeRWgbNEkqO", new Scram("user", "pencil", NONCE).clientFirstMessage());
    }

    @Test
    void clientFinalMessageMatchesRfc() {
        Scram scram = new Scram("user", "pencil", NONCE);
        scram.clientFirstMessage();
        assertEquals("c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
                + "p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=", scram.clientFinalMessage(SERVER_FIRST));
    }

    @Test
    void acceptsTheRfcServerSignature() {
        Scram scram = new Scram("user", "pencil", NONCE);
        scram.clientFirstMessage();
        scram.clientFinalMessage(SERVER_FIRST);
        assertDoesNotThrow(() -> scram.verifyServerFinal("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
    }

    @Test
    void rejectsAWrongServerSignature() {
        Scram scram = new Scram("user", "pencil", NONCE);
        scram.clientFirstMessage();
        scram.clientFinalMessage(SERVER_FIRST);
        assertThrows(IllegalStateException.class,
                () -> scram.verifyServerFinal("v=AAAATRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
    }

    @Test
    void rejectsAServerNonceThatDoesNotExtendOurs() {
        Scram scram = new Scram("user", "pencil", NONCE);
        scram.clientFirstMessage();
        assertThrows(IllegalStateException.class,
                () -> scram.clientFinalMessage("r=someoneElse,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096"));
    }

    @Test
    void reportsAServerError() {
        Scram scram = new Scram("user", "pencil", NONCE);
        scram.clientFirstMessage();
        scram.clientFinalMessage(SERVER_FIRST);
        assertThrows(IllegalStateException.class, () -> scram.verifyServerFinal("e=invalid-proof"));
    }
}
