#pragma once

#include <cstdint>
#include <expected>
#include <memory>

#include "api.hpp"
#include "tuning.hpp"
#include "types.hpp"

namespace feed::pg
{

struct Target
{
    String host;
    String port;
    String user;
    String password;
    String database;
};

std::expected<Target, String> parse_url(View url);

void append_startup(String &out, View user, View database, const tuning::Setting *settings, size_t settings_count);
void append_parse(String &out, View name, View sql, const uint32_t *types, size_t type_count);

struct Value
{
    const char *data;
    int32_t size;
};

void append_bind(String &out, View statement, const Value *params, size_t param_count);
void append_execute(String &out);
void append_sync(String &out);
void append_password(String &out, View password);
void append_sasl_initial(String &out, View mechanism, View data);
void append_sasl_response(String &out, View data);

struct Frame
{
    char type = 0;
    View body;
};

enum class FrameStatus
{
    Ok,
    Incomplete,
    Bad
};

FrameStatus next_frame(View in, size_t &off, Frame &frame);

struct ErrorInfo
{
    String sqlstate;
    String message;
};

ErrorInfo parse_error(View body);

int64_t command_rows(View tag);

bool split_row(View body, View *columns, size_t count);

bool decode_int(View column, int64_t &out);

void append_timestamp_text(String &out, int64_t micros);

bool decode_post(View row, Post &post);
bool decode_created(View row, NewPost &created);

enum class Fate : unsigned char
{
    Deliver,
    Error,
    Replay,
};

// `error_at` is the failed query's index, or `count` for none. After an error the server skips the rest of the batch
// and rolls it back: reads before it keep their answers, everything else is replayed, except the failing query
// when the error is its own (data exception, constraint violation) or it ran alone.
void batch_fates(const DbKind *kinds, size_t count, size_t error_at, const ErrorInfo *error, Fate *fates);

class Scram
{
  public:
    Scram(View user, View password, View nonce);

    [[nodiscard]] String first_message() const;

    std::expected<String, String> final_message(View server_first);

    [[nodiscard]] bool verify(View server_final) const;

  private:
    String password_;
    String nonce_;
    String first_bare_;
    unsigned char server_signature_[32] = {};
    bool answered_ = false;
};

String random_nonce();

} // namespace feed::pg
