using System.Buffers.Binary;
using System.Security.Cryptography;
using System.Text;

namespace Feed.Pg;

/// <summary>
/// Sets up one Postgres connection: logs in (SCRAM-SHA-256 or trust) and prepares statements, blocking. Then
/// <see cref="Detach"/> hands the socket to an event loop, which drives the extended protocol itself.
/// </summary>
public sealed class PgConnection : IDisposable {
    /// <summary>The byte pipe to the server.</summary>
    private readonly IPgTransport _transport;
    /// <summary>Splits the server's bytes into messages.</summary>
    private readonly PgMessageReader _reader;
    /// <summary>Builds outgoing messages.</summary>
    private readonly PgWriter _writer = new();
    /// <summary>Set once the socket has been handed over or closed.</summary>
    private bool _done;

    /// <summary>Creates a connection over <paramref name="transport"/>. Login happens in <see cref="Open"/>.</summary>
    private PgConnection(IPgTransport transport) {
        _transport = transport;
        _reader = new PgMessageReader(transport);
    }

    /// <summary>Opens a TCP connection, logs in and applies the session settings, which go in the startup packet.</summary>
    public static PgConnection Connect(PgConnectionString cs, string applicationName,
        IReadOnlyList<KeyValuePair<string, string>> sessionSettings) {
        SocketTransport t = SocketTransport.Connect(cs.Host, cs.Port);
        try {
            return Open(t, cs, applicationName, sessionSettings, null);
        } catch {
            t.Close();
            throw;
        }
    }

    /// <summary>Logs in over an existing transport. <paramref name="clientNonce"/> is for tests; null picks a random one.</summary>
    internal static PgConnection Open(IPgTransport transport, PgConnectionString cs, string applicationName,
        IReadOnlyList<KeyValuePair<string, string>> sessionSettings, string? clientNonce) {
        PgConnection c = new(transport);
        c._writer.Clear();
        PgEncoder.Startup(c._writer, cs.User, cs.Database, applicationName, sessionSettings);
        transport.Write(c._writer.Written);

        PgScram? scram = null;
        while (true) {
            byte type = c._reader.Next(out ReadOnlySpan<byte> body);
            switch ((char)type) {
                case 'R':
                    scram = c.HandleAuth(body, cs, scram, clientNonce);
                    break;
                case 'E':
                    throw new PgException(PgMessages.ParseError(body));
                case 'Z':
                    return c;
                default:
                    // ParameterStatus, BackendKeyData and notices: nothing here needs them.
                    break;
            }
        }
    }

    /// <summary>Replies to one Authentication message and returns the SCRAM state to keep. Only SCRAM-SHA-256 and trust are accepted.</summary>
    private PgScram? HandleAuth(ReadOnlySpan<byte> body, PgConnectionString cs, PgScram? scram, string? clientNonce) {
        int code = BinaryPrimitives.ReadInt32BigEndian(body);
        switch (code) {
            case 0:
                return scram;
            case 10:
                if (!SaslMechanisms(body[4..]).Contains("SCRAM-SHA-256")) {
                    throw new PgException("server offers no SCRAM-SHA-256");
                }

                scram = new PgScram(cs.User, cs.Password, clientNonce ?? NewNonce());
                _writer.Clear();
                PgEncoder.SaslInitialResponse(_writer, "SCRAM-SHA-256", Encoding.UTF8.GetBytes(scram.ClientFirst));
                _transport.Write(_writer.Written);
                return scram;
            case 11:
                if (scram is null) {
                    throw new PgException("unexpected SASLContinue");
                }

                _writer.Clear();
                PgEncoder.SaslResponse(_writer, Encoding.UTF8.GetBytes(scram.ClientFinal(Encoding.UTF8.GetString(body[4..]))));
                _transport.Write(_writer.Written);
                return scram;
            case 12:
                if (scram is null) {
                    throw new PgException("unexpected SASLFinal");
                }

                scram.VerifyServerFinal(Encoding.UTF8.GetString(body[4..]));
                return scram;
            default:
                throw new PgException("unsupported authentication method " + code);
        }
    }

    /// <summary>Reads the NUL-terminated mechanism names of an AuthenticationSASL message.</summary>
    private static List<string> SaslMechanisms(ReadOnlySpan<byte> list) {
        List<string> result = [];
        while (!list.IsEmpty && list[0] != 0) {
            int end = list.IndexOf((byte)0);
            result.Add(Encoding.ASCII.GetString(list[..end]));
            list = list[(end + 1)..];
        }
        return result;
    }

    /// <summary>A random client nonce for SCRAM, from 18 random bytes in base64.</summary>
    private static string NewNonce() => Convert.ToBase64String(RandomNumberGenerator.GetBytes(18));

    /// <summary>Prepares a named statement and waits for the server. Call it before <see cref="Detach"/>.</summary>
    /// <exception cref="PgException">The server rejected the statement.</exception>
    public void Prepare(string statementName, string sql, ReadOnlySpan<uint> paramOids) {
        if (_done) {
            throw new InvalidOperationException("the connection was handed over or closed");
        }

        _writer.Clear();
        PgEncoder.Parse(_writer, statementName, sql, paramOids);
        PgEncoder.Sync(_writer);
        _transport.Write(_writer.Written);
        PgError? error = null;
        while (true) {
            byte type = _reader.Next(out ReadOnlySpan<byte> body);
            if (type == 'Z') {
                if (error is { } e) {
                    throw new PgException(e);
                }

                return;
            }
            if (type == 'E') {
                error = PgMessages.ParseError(body);
            }
        }
    }

    /// <summary>
    /// Hands the logged-in, prepared socket to a caller that drives the protocol itself, such as an event loop. This
    /// object is unusable afterwards. Throws if it was already handed over or closed, or if the server sent bytes
    /// nobody read.
    /// </summary>
    public System.Net.Sockets.Socket Detach() {
        if (_done || _transport is not SocketTransport t) {
            throw new InvalidOperationException("only an open socket connection can be handed over");
        }

        if (_reader.Buffered != 0) {
            throw new PgException($"{_reader.Buffered} unread bytes after setup");
        }

        _done = true;
        return t.Socket;
    }

    /// <summary>Sends Terminate and closes the transport, unless the socket was handed over.</summary>
    public void Dispose() {
        if (_done) {
            return;
        }

        _done = true;
        try {
            _writer.Clear();
            PgEncoder.Terminate(_writer);
            _transport.Write(_writer.Written);
        } catch (Exception) {
            // Closing anyway.
        }

        try {
            _transport.Close();
        } catch (Exception) {
            // Already closed.
        }
    }
}
