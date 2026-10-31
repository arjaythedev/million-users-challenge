"""The API contract: replies, and the single table mapping domain failures to status and message."""
from typing import NamedTuple

from ..domain import errors


class Reply(NamedTuple):
    status: int
    body: bytes  # compact JSON, UTF-8, keys in spec order


def _error_body(message: str) -> bytes:
    return b'{"error":"' + message.encode() + b'"}'


ERRORS: dict[type[errors.DomainError], Reply] = {
    errors.InvalidPostId: Reply(400, _error_body("invalid post id")),
    errors.PostNotFound: Reply(404, _error_body("post not found")),
    errors.MissingBearerToken: Reply(401, _error_body("missing bearer token")),
    errors.InvalidToken: Reply(401, _error_body("invalid or expired token")),
    errors.InvalidTokenPayload: Reply(401, _error_body("invalid token payload")),
    errors.MalformedJson: Reply(400, _error_body("malformed JSON body")),
    errors.BodyRequired: Reply(400, _error_body("body is required")),
    errors.BodyTooLong: Reply(400, _error_body("body must be at most 500 characters")),
}

# Failures that are not the domain's: the transport found no route, or something broke.
NOT_FOUND = Reply(404, _error_body("not found"))
INTERNAL_ERROR = Reply(500, _error_body("internal server error"))


def reply_for(error: errors.DomainError) -> Reply:
    return ERRORS[type(error)]
