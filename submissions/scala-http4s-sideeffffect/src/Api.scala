package feed

import cats.data.Kleisli
import cats.effect.IO
import fs2.Chunk
import io.circe.syntax.*
import io.circe.{Encoder, Json, Printer, jawn}
import org.http4s.*
import org.http4s.dsl.io.*
import org.http4s.headers.{`Accept-Encoding`, `Content-Encoding`, `Content-Type`}
import org.typelevel.ci.*
import skunk.SqlState
import skunk.data.Completion

import scala.concurrent.duration.*

/** The five endpoints of SPEC.md. Each request runs exactly one statement, in autocommit, on a pooled session: reads on
  * the read sessions, inserts on the write sessions.
  */
final class Api(db: Db, jwt: Jwt, startedAt: FiniteDuration):

  // Responses

  private val jsonType = `Content-Type`(MediaType.application.json)

  /** Compact JSON, gzipped when the client accepts it and the body is big enough to gain from it. */
  private def json[A: Encoder](request: Request[IO], status: Status, value: A): IO[Response[IO]] = IO {
    val plain = Printer.noSpaces.printToByteBuffer(value.asJson)
    val bytes = new Array[Byte](plain.remaining)
    plain.get(bytes)
    val gzip = bytes.length >= Gzip.MinSize &&
      request.headers.get[`Accept-Encoding`].exists(_.satisfiedBy(ContentCoding.gzip))
    val body = if gzip then Gzip.compress(bytes) else bytes
    val response = Response[IO](status).withEntity(Chunk.array(body)).putHeaders(jsonType)
    if gzip then response.putHeaders(`Content-Encoding`(ContentCoding.gzip)) else response
  }

  private def error(request: Request[IO], e: ApiError): IO[Response[IO]] =
    json(request, e.status, ApiError.body(e.message))

  // Request parts

  private def now: IO[Long] = IO.realTime.map(_.toSeconds)

  /** Auth comes first on every authenticated route, before the id or the body is looked at. */
  private def caller(request: Request[IO]): IO[Caller] =
    request.headers.get(ci"Authorization").map(_.head.value) match
      case Some(header) if header.startsWith("Bearer ") =>
        now.flatMap(t => IO.fromEither(jwt.verify(header.substring(7), t)))
      case _ => IO.raiseError(ApiError.missingToken)

  /** A positive integer id: digits only, so `0`, `-1`, `abc` and `1.5` are all 400. */
  private def postId(segment: String): IO[Long] =
    IO.fromOption(
      Option
        .when(segment.nonEmpty && segment.length <= 18 && segment.forall(c => c >= '0' && c <= '9'))(segment.toLong)
        .filter(_ > 0)
    )(ApiError.invalidPostId)

  /** `{"body":"..."}`: a string, non-empty and at most 500 characters after trimming. */
  private def postBody(request: Request[IO]): IO[String] =
    request.body.compile.to(Array).flatMap { bytes =>
      IO.fromEither(
        for
          json <- jawn.parseByteArray(bytes).left.map(_ => ApiError.malformedJson)
          body <- json.hcursor.get[String]("body").map(_.strip).left.map(_ => ApiError.bodyRequired)
          _ <- Either.cond(body.nonEmpty, (), ApiError.bodyRequired)
          _ <- Either.cond(body.codePointCount(0, body.length) <= 500, (), ApiError.bodyTooLong)
        yield body
      )
    }

  // Endpoints

  private def health(request: Request[IO]): IO[Response[IO]] =
    db.read
      .use(_.unique(Sql.ping))
      .timeout(5.seconds)
      .flatMap(_ => IO.monotonic)
      .flatMap(t =>
        json(request, Status.Ok, Json.obj("status" := "ok", "db" := "ok", "uptime_s" := (t - startedAt).toSeconds))
      )
      .handleErrorWith(e =>
        json(
          request,
          Status.ServiceUnavailable,
          Json.obj("status" := "degraded", "db" := "unreachable", "error" := String.valueOf(e.getMessage))
        )
      )

  private val routes: HttpRoutes[IO] = HttpRoutes.of[IO] {
    case request @ GET -> Root / "health" => health(request)

    case request @ GET -> Root / "feed" =>
      db.read.use(_.execute(Sql.feed)).flatMap(posts => json(request, Status.Ok, Json.obj("posts" := posts)))

    case request @ GET -> Root / "posts" / id =>
      for
        id <- postId(id)
        post <- db.read.use(_.option(Sql.postById)(id)).flatMap(IO.fromOption(_)(ApiError.postNotFound))
        response <- json(request, Status.Ok, Json.obj("post" := post))
      yield response

    case request @ POST -> Root / "posts" =>
      for
        caller <- caller(request)
        body <- postBody(request)
        (id, createdAt) <- db.write.use(_.unique(Sql.insertPost)((caller.id, body)))
        post = Post(id, body, createdAt, caller.username, 0)
        response <- json(request, Status.Created, Json.obj("post" := post))
      yield response

    case request @ POST -> Root / "posts" / id / "like" =>
      for
        caller <- caller(request)
        id <- postId(id)
        completion <- db.write
          .use(_.execute(Sql.insertLike)((caller.id, id)))
          .adaptError { case SqlState.ForeignKeyViolation(_) => ApiError.postNotFound }
        inserted = completion == Completion.Insert(1)
        response <- json(
          request,
          if inserted then Status.Created else Status.Ok,
          Like(liked = true, already_liked = !inserted, post_id = id)
        )
      yield response
  }

  val app: HttpApp[IO] = Kleisli { request =>
    routes(request)
      .getOrElseF(IO.raiseError(ApiError.notFound))
      .handleErrorWith {
        case e: ApiError => error(request, e)
        case e => IO.consoleForIO.errorln(s"${request.method} ${request.uri}: $e") *> error(request, ApiError.internal)
      }
  }
