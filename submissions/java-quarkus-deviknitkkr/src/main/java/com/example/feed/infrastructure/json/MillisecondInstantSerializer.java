package com.example.feed.infrastructure.json;

import com.fasterxml.jackson.core.JsonGenerator;
import com.fasterxml.jackson.databind.JsonSerializer;
import com.fasterxml.jackson.databind.SerializerProvider;
import java.io.IOException;
import java.time.Instant;
import java.time.format.DateTimeFormatter;
import java.time.format.DateTimeFormatterBuilder;

/** Fixed UTC/millisecond format without the general-purpose zoned pattern formatter. */
public final class MillisecondInstantSerializer extends JsonSerializer<Instant> {
    private static final DateTimeFormatter FORMAT = new DateTimeFormatterBuilder().appendInstant(3).toFormatter();

    @Override
    public void serialize(Instant value, JsonGenerator output, SerializerProvider provider) throws IOException {
        output.writeString(FORMAT.format(value));
    }
}
