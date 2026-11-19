package com.example.feed.shared.error;

import java.util.Locale;
import jakarta.ws.rs.WebApplicationException;
import jakarta.ws.rs.core.MediaType;
import jakarta.ws.rs.core.Response;
import jakarta.ws.rs.ext.ExceptionMapper;
import jakarta.ws.rs.ext.Provider;
import org.jboss.logging.Logger;

@Provider
public class GlobalExceptionHandler implements ExceptionMapper<Exception> {
    private static final Logger LOG = Logger.getLogger(GlobalExceptionHandler.class);

    @Override
    public Response toResponse(Exception exception) {
        int status = 500;
        String message = "internal server error";
        if (exception instanceof ApiException api) {
            status = api.getStatus();
            message = api.getMessage();
        } else if (exception instanceof WebApplicationException web) {
            status = web.getResponse().getStatus();
            message = reasonPhrase(status);
        } else {
            LOG.error("Request failed", exception);
        }
        return Response.status(status).type(MediaType.APPLICATION_JSON_TYPE).entity(new ErrorBody(message)).build();
    }

    private static String reasonPhrase(int status) {
        if (status == 404) {
            return "not found";
        }
        Response.Status standardStatus = Response.Status.fromStatusCode(status);
        return standardStatus == null ? "request failed" : standardStatus.getReasonPhrase().toLowerCase(Locale.ROOT);
    }
}
