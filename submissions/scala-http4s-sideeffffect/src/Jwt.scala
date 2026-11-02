package feed

import io.circe.jawn

import java.nio.charset.StandardCharsets.UTF_8
import java.security.MessageDigest
import java.util.Base64
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

/** The caller named by a valid token: `sub` as the user id, `username` as-is (it isn't looked up). */
final case class Caller(id: Long, username: String)

/** HS256 verification with the JDK's `Mac`. Every request verifies its token again: there is no cache (rule 7). */
final class Jwt(secret: String):
  private val prototype =
    val mac = Mac.getInstance("HmacSHA256")
    mac.init(SecretKeySpec(secret.getBytes(UTF_8), "HmacSHA256"))
    mac

  // A clone of the initialised Mac is cheap and needs no synchronisation.
  private def hmac(data: String): Array[Byte] =
    prototype.clone().asInstanceOf[Mac].doFinal(data.getBytes(UTF_8))

  private def unb64(s: String): Option[Array[Byte]] =
    try Some(Base64.getUrlDecoder.decode(s))
    catch case _: IllegalArgumentException => None

  /** `invalidToken` unless the token is well formed, HS256, correctly signed, and has a numeric `exp` in the future
    * (and `nbf`, if present, not in the future); then `invalidPayload` unless `sub` is a positive integer string and
    * `username` a string.
    */
  def verify(token: String, nowSeconds: Long): Either[ApiError, Caller] =
    // -1 keeps trailing empty parts, so "h.p.s." is four parts (and rejected), not three.
    val claims = token.split("\\.", -1) match
      case Array(h, p, s) =>
        for
          header <- unb64(h).flatMap(bytes => jawn.parseByteArray(bytes).toOption)
          if header.hcursor.get[String]("alg").contains("HS256")
          signature <- unb64(s)
          if MessageDigest.isEqual(signature, hmac(s"$h.$p"))
          claims <- unb64(p).flatMap(bytes => jawn.parseByteArray(bytes).toOption).flatMap(_.asObject)
          exp <- claims("exp").flatMap(_.asNumber).map(_.toDouble)
          if nowSeconds < exp
          if claims("nbf").forall(_.asNumber.exists(_.toDouble <= nowSeconds))
        yield claims
      case _ => None
    for
      claims <- claims.toRight(ApiError.invalidToken)
      caller <- (for
        sub <- claims("sub").flatMap(_.asString)
        if sub.nonEmpty && sub.length <= 18 && sub.forall(c => c >= '0' && c <= '9')
        id = sub.toLong
        if id > 0
        username <- claims("username").flatMap(_.asString)
      yield Caller(id, username)).toRight(ApiError.invalidPayload)
    yield caller
