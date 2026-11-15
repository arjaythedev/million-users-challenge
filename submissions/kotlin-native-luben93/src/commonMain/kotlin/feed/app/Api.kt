package feed.app

import feed.auth.Jwt
import feed.auth.positiveId
import feed.db.Db
import feed.http.Exchange
import feed.http.Method
import feed.json.JsonName
import feed.json.JsonReader
import feed.json.JsonValue
import feed.json.JsonWriter
import feed.pg.Row
import feed.platform.nanoTime
import feed.stats.EP_CREATE
import feed.stats.EP_FEED
import feed.stats.EP_HEALTH
import feed.stats.EP_LIKE
import feed.stats.EP_POST

/** The feed API (SPEC.md): five endpoints, exact JSON key order, the SPEC's error messages. */
object Api {
    private val started = nanoTime()

    private val POSTS = JsonName("posts")
    private val POST = JsonName("post")
    private val ID = JsonName("id")
    private val BODY = JsonName("body")
    private val CREATED_AT = JsonName("created_at")
    private val AUTHOR = JsonName("author")
    private val LIKE_COUNT = JsonName("like_count")
    private val LIKED = JsonName("liked")
    private val ALREADY_LIKED = JsonName("already_liked")
    private val POST_ID = JsonName("post_id")
    private val STATUS = JsonName("status")
    private val DB = JsonName("db")
    private val UPTIME_S = JsonName("uptime_s")
    private val ERROR = JsonName("error")

    private const val NOT_FOUND = "not found"
    private const val INVALID_ID = "invalid post id"
    private const val POST_NOT_FOUND = "post not found"
    private const val INTERNAL = "internal server error"

    suspend fun handle(ex: Exchange) {
        val path = ex.path
        when {
            path == "/feed" && ex.method == Method.GET -> feed(ex)
            path == "/health" && ex.method == Method.GET -> health(ex)
            path == "/posts" && ex.method == Method.POST -> create(ex)
            path.startsWith("/posts/") -> {
                val slash = path.indexOf('/', 7)
                val idText = if (slash < 0) path.substring(7) else path.substring(7, slash)
                val rest = if (slash < 0) "" else path.substring(slash)
                when {
                    idText.isEmpty() -> ex.sendError(404, NOT_FOUND)
                    rest.isEmpty() && ex.method == Method.GET -> getPost(ex, idText)
                    rest == "/like" && ex.method == Method.POST -> like(ex, idText)
                    else -> ex.sendError(404, NOT_FOUND)
                }
            }
            else -> ex.sendError(404, NOT_FOUND)
        }
    }

    private suspend fun health(ex: Exchange) {
        ex.endpoint = EP_HEALTH
        val r = Db.run(Db.job(Db.PING, ex))
        val j = ex.json
        j.beginObject()
        val err = r.error
        if (err == null) {
            j.name(STATUS); j.string("ok")
            j.name(DB); j.string("ok")
            j.name(UPTIME_S); j.number((nanoTime() - started) / 1_000_000_000)
            j.endObject()
            ex.send(200)
        } else {
            j.name(STATUS); j.string("degraded")
            j.name(DB); j.string("unreachable")
            j.name(ERROR); j.string(err.message)
            j.endObject()
            ex.send(503)
        }
    }

    private suspend fun feed(ex: Exchange) {
        ex.endpoint = EP_FEED
        val r = Db.run(Db.job(Db.FEED, ex))
        if (r.error != null) return ex.sendError(500, INTERNAL)
        val j = ex.json
        j.beginObject()
        j.name(POSTS)
        j.beginArray()
        for (i in 0 until r.rowCount) post(j, r.row(i))
        j.endArray()
        j.endObject()
        ex.send(200)
    }

    private suspend fun getPost(ex: Exchange, idText: String) {
        ex.endpoint = EP_POST
        val id = positiveId(idText)
        if (id <= 0) return ex.sendError(400, INVALID_ID)
        val r = Db.run(Db.job(Db.POST, ex).int8(id))
        if (r.error != null) return ex.sendError(500, INTERNAL)
        if (r.rowCount == 0) return ex.sendError(404, POST_NOT_FOUND)
        val j = ex.json
        j.beginObject()
        j.name(POST)
        post(j, r.row(0))
        j.endObject()
        ex.send(200)
    }

    private suspend fun create(ex: Exchange) {
        ex.endpoint = EP_CREATE
        Jwt.authenticate(ex)?.let { return ex.sendError(401, it) }
        val v = JsonReader.parse(ex.body) ?: return ex.sendError(400, "malformed JSON body")
        val raw = ((v as? JsonValue.Obj)?.get("body") as? JsonValue.Str)?.value ?: return ex.sendError(400, "body is required")
        val body = raw.trim()
        if (body.isEmpty()) return ex.sendError(400, "body is required")
        if (codePoints(body) > 500) return ex.sendError(400, "body must be at most 500 characters")
        val user = ex.viewer
        val author = ex.username!!
        val r = Db.run(Db.job(Db.INSERT_POST, ex).int8(user).text(body))
        if (r.error != null || r.rowCount == 0) return ex.sendError(500, INTERNAL)
        val row = r.row(0)
        val j = ex.json
        j.beginObject()
        j.name(POST)
        j.beginObject()
        j.name(ID); j.number(row.int8(0))
        j.name(BODY); j.string(body)
        j.name(CREATED_AT); j.timestamp(row.micros(1))
        j.name(AUTHOR); j.string(author)
        j.name(LIKE_COUNT); j.number(0)
        j.endObject()
        j.endObject()
        ex.send(201)
    }

    private suspend fun like(ex: Exchange, idText: String) {
        ex.endpoint = EP_LIKE
        Jwt.authenticate(ex)?.let { return ex.sendError(401, it) } // auth before the id (SPEC.md)
        val id = positiveId(idText)
        if (id <= 0) return ex.sendError(400, INVALID_ID)
        val r = Db.run(Db.job(Db.INSERT_LIKE, ex).int8(ex.viewer).int8(id))
        val err = r.error
        if (err != null) return ex.sendError(if (err.code == "23503") 404 else 500, if (err.code == "23503") POST_NOT_FOUND else INTERNAL)
        val inserted = r.affected > 0
        val j = ex.json
        j.beginObject()
        j.name(LIKED); j.bool(true)
        j.name(ALREADY_LIKED); j.bool(!inserted)
        j.name(POST_ID); j.number(id)
        j.endObject()
        ex.send(if (inserted) 201 else 200)
    }

    /** {"id":..,"body":..,"created_at":..,"author":..,"like_count":..} from a post row (binary columns). */
    private fun post(j: JsonWriter, row: Row) {
        j.beginObject()
        j.name(ID); j.number(row.int8(0))
        j.name(BODY); j.text(row, 1)
        j.name(CREATED_AT); j.timestamp(row.micros(2))
        j.name(AUTHOR); j.text(row, 3)
        j.name(LIKE_COUNT); j.number(row.int4(4).toLong())
        j.endObject()
    }

    /** Unicode code points (what Postgres's char_length counts), not UTF-16 units. */
    private fun codePoints(s: String): Int {
        var n = 0
        for (c in s) if (!c.isLowSurrogate()) n++
        return n
    }
}
