package feed

import cats.effect.*
import cats.effect.std.Console
import cats.syntax.all.*
import com.comcast.ip4s.{Host, Port}
import fs2.io.net.SocketOption
import org.http4s.HttpApp
import org.http4s.ember.server.EmberServerBuilder
import org.typelevel.otel4s.metrics.MeterProvider
import org.typelevel.otel4s.trace.TracerProvider
import skunk.Session
import skunk.telemetry.{QueryCaptureConfig, TelemetryConfig}

import java.net.URI
import scala.concurrent.duration.*

final case class Config(
    databaseUrl: String,
    jwtSecret: String,
    host: Host,
    ports: List[Port],
    readSessions: Int,
    writeSessions: Int,
    maxConnections: Option[Int]
)

object Config:
  def fromEnv: IO[Config] = IO {
    def env(name: String) = sys.env.get(name).filter(_.nonEmpty)
    def required(name: String) = env(name).getOrElse(sys.error(s"$name is not set"))
    def int(name: String, default: Int) = env(name).flatMap(_.toIntOption).getOrElse(default)
    Config(
      databaseUrl = required("DATABASE_URL"),
      jwtSecret = required("JWT_SECRET"),
      host = env("HOST").flatMap(Host.fromString).getOrElse(Host.fromString("0.0.0.0").get),
      ports = List(
        env("PORT").flatMap(Port.fromString).getOrElse(Port.fromInt(80).get).some,
        env("EXTRA_PORT").flatMap(Port.fromString)
      ).flatten.distinct,
      readSessions = int("DB_READ_SESSIONS", 128),
      writeSessions = int("DB_WRITE_SESSIONS", 32),
      maxConnections = env("MAX_CONNECTIONS").flatMap(_.toIntOption)
    )
  }

object Main extends IOApp.Simple:

  /** Heap one idle keep-alive connection costs in Ember: 38.1 KB measured (FINDINGS.md), rounded up. */
  private val heapPerConnection = 40L * 1024

  /** Connections per port that fit in 80% of the heap; the rest is for requests in flight and their garbage. Past
    * this, Ember stops accepting and new connections wait in the kernel's backlog: clients see slow connects, but
    * the process doesn't run out of memory (and a crashed server isn't restarted).
    */
  private def connectionsPerPort(ports: Int): Int =
    (Runtime.getRuntime.maxMemory * 8 / 10 / heapPerConnection / ports).toInt.max(1)

  // Under load the starvation checker's warnings would only add noise.
  override protected def runtimeConfig =
    super.runtimeConfig.copy(cpuStarvationCheckInitialDelay = Duration.Inf)

  /** One compute thread per core: Postgres is on another machine, so the whole box is ours. */
  override protected def computeWorkerThreadCount: Int =
    sys.env.get("COMPUTE_THREADS").flatMap(_.toIntOption).getOrElse(Runtime.getRuntime.availableProcessors)

  // No tracing or metrics: skunk instruments every statement through otel4s, and the no-op providers plus disabled
  // pool and protocol spans make that free.
  private given TracerProvider[IO] = TracerProvider.noop[IO]
  private given MeterProvider[IO] = MeterProvider.noop[IO]

  /** `postgres://user:password@host:port/db` as a skunk session. */
  private def session(databaseUrl: String): Resource[IO, Session[IO]] =
    val uri = URI(databaseUrl)
    val (user, password) = Option(uri.getUserInfo).map(_.split(":", 2)) match
      case Some(Array(u, p)) => (u, p)
      case Some(Array(u))    => (u, "")
      case _                 => ("postgres", "")
    Session
      .Builder[IO]
      .withHost(uri.getHost)
      .withPort(if uri.getPort > 0 then uri.getPort else 5432)
      .withUserAndPassword(user, password)
      .withDatabase(uri.getPath.stripPrefix("/"))
      // Session-level settings (rule 2): one generic plan per prepared statement instead of five custom plans first,
      // and no JIT compilation for these short index lookups.
      .withConnectionParameters(
        Session.DefaultConnectionParameters ++ Map("plan_cache_mode" -> "force_generic_plan", "jit" -> "off")
      )
      .withTelemetryConfig(
        TelemetryConfig.default
          .withCaptureQuery(QueryCaptureConfig.disabled)
          .withPoolSpans(TelemetryConfig.PoolSpans.Disabled)
          .withProtocolSpans(TelemetryConfig.ProtocolSpans.Disabled)
      )
      .single

  /** One Ember server on `port`. Both ports share the app, and so the sessions. */
  private def serve(config: Config, app: HttpApp[IO])(port: Port): Resource[IO, Unit] =
    for
      _ <- EmberServerBuilder
        .default[IO]
        .withHost(config.host)
        .withPort(port)
        .withHttpApp(app)
        .withMaxConnections(config.maxConnections.getOrElse(connectionsPerPort(config.ports.size)))
        // Send small responses at once instead of waiting for the client's delayed ACK (Nagle's algorithm).
        .withAdditionalSocketOptions(List(SocketOption.noDelay(true)))
        // Users idle up to ~40 s between pages and must be kept for at least 75 s. No idle timeout at all: a finite
        // one races a timer against every read, which costs CPU per request and heap per idle connection.
        .withIdleTimeout(Duration.Inf)
        // fs2 gives every socket an 8 KB read buffer whatever this says; asking for more would grow it.
        .withReceiveBufferSize(8 * 1024)
        .withShutdownTimeout(1.second)
        .build
      raised <- Resource.eval(ListenBacklog.raise(port.value, 65535))
      _ <- Resource.eval(IO.unlessA(raised)(Console[IO].errorln(s"could not raise the listen backlog of port $port")))
    yield ()

  def run: IO[Unit] =
    val server = for
      config <- Resource.eval(Config.fromEnv)
      startedAt <- Resource.eval(IO.monotonic)
      // Inserts get their own sessions, so a burst of reads can't queue them (or the other way round).
      reads <- SessionPool(config.readSessions, session(config.databaseUrl))
      writes <- SessionPool(config.writeSessions, session(config.databaseUrl))
      api = Api(Db(reads, writes), Jwt(config.jwtSecret), startedAt)
      _ <- config.ports.traverse_(serve(config, api.app))
    yield ()
    server.useForever.void
