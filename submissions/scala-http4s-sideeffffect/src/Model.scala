package feed

import io.circe.{Encoder, Json}
import org.http4s.Status

import java.time.format.DateTimeFormatter
import java.time.{Instant, ZoneOffset}
import scala.util.control.NoStackTrace

/** UTC with exactly 3 fractional digits and a `Z`. `SSS` prints the first three digits, so it truncates (not rounds).
  */
opaque type Timestamp = Instant
object Timestamp:
  private val format = DateTimeFormatter.ofPattern("yyyy-MM-dd'T'HH:mm:ss.SSS'Z'").withZone(ZoneOffset.UTC)
  def apply(instant: Instant): Timestamp = instant
  given Encoder[Timestamp] = Encoder.encodeString.contramap(format.format)

/** A post as the API returns it. The derived encoder keeps the field order, which the spec fixes. */
final case class Post(id: Long, body: String, created_at: Timestamp, author: String, like_count: Int)
    derives Encoder.AsObject

final case class Like(liked: Boolean, already_liked: Boolean, post_id: Long) derives Encoder.AsObject

/** A failed request, answered as `{"error":<message>}`. */
final case class ApiError(status: Status, message: String) extends Exception(message) with NoStackTrace
object ApiError:
  val notFound: ApiError = ApiError(Status.NotFound, "not found")
  val invalidPostId: ApiError = ApiError(Status.BadRequest, "invalid post id")
  val postNotFound: ApiError = ApiError(Status.NotFound, "post not found")
  val missingToken: ApiError = ApiError(Status.Unauthorized, "missing bearer token")
  val invalidToken: ApiError = ApiError(Status.Unauthorized, "invalid or expired token")
  val invalidPayload: ApiError = ApiError(Status.Unauthorized, "invalid token payload")
  val malformedJson: ApiError = ApiError(Status.BadRequest, "malformed JSON body")
  val bodyRequired: ApiError = ApiError(Status.BadRequest, "body is required")
  val bodyTooLong: ApiError = ApiError(Status.BadRequest, "body must be at most 500 characters")
  val internal: ApiError = ApiError(Status.InternalServerError, "internal server error")

  def body(message: String): Json = Json.obj("error" -> Json.fromString(message))
