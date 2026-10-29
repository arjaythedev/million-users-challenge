
#include <gtest/gtest.h>

#include <vector>

#include "pg_wire.hpp"
#include "test_util.hpp"

namespace feed::test
{
namespace
{

Str bytes(std::initializer_list<int> list)
{
    Str s;
    for (const int b : list)
        s.push_back(static_cast<char>(b));
    return s;
}

void be32(Str &s, uint32_t v)
{
    for (int shift : {24, 16, 8, 0})
        s.push_back(static_cast<char>(v >> shift));
}

void be16(Str &s, unsigned v)
{
    s.push_back(static_cast<char>(v >> 8));
    s.push_back(static_cast<char>(v));
}

Str data_row(std::initializer_list<Str> columns)
{
    Str body;
    be16(body, static_cast<unsigned>(columns.size()));
    for (const Str &c : columns)
    {
        be32(body, static_cast<uint32_t>(c.size()));
        body.append(c.data(), c.size());
    }
    return body;
}

Str int_be(int64_t v, int width)
{
    Str s;
    for (int i = width - 1; i >= 0; i--)
        s.push_back(static_cast<char>(static_cast<uint64_t>(v) >> (8 * i)));
    return s;
}

int64_t micros_2000(int64_t days, int64_t secs, int64_t micros)
{
    return (days * 86400 + secs) * 1'000'000 + micros;
}

TEST(PgUrl, AllPartsPercentDecoded)
{
    const auto t = pg::parse_url("postgres://feed:p%40ss%3Aword@db.example:6432/feed_bench?sslmode=disable");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->user, "feed");
    EXPECT_EQ(t->password, "p@ss:word");
    EXPECT_EQ(t->host, "db.example");
    EXPECT_EQ(t->port, "6432");
    EXPECT_EQ(t->database, "feed_bench");
}

TEST(PgUrl, DefaultsForPortHostAndDatabase)
{
    const auto t = pg::parse_url("postgresql://feed@/");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->host, "localhost");
    EXPECT_EQ(t->port, "5432");
    EXPECT_EQ(t->database, "feed");
    EXPECT_TRUE(t->password.empty());
}

TEST(PgUrl, Ipv6Host)
{
    const auto t = pg::parse_url("postgres://u:p@[::1]:5433/d");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->host, "::1");
    EXPECT_EQ(t->port, "5433");
}

TEST(PgUrl, PasswordMayContainAtSigns)
{
    const auto t = pg::parse_url("postgres://u:a@b@host/d");
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(t->password, "a@b");
    EXPECT_EQ(t->host, "host");
}

TEST(PgUrl, Rejects)
{
    EXPECT_FALSE(pg::parse_url("mysql://u@h/d").has_value());
    EXPECT_FALSE(pg::parse_url("postgres://h/d").has_value());
    EXPECT_FALSE(pg::parse_url("postgres://u@h:abc/d").has_value());
    EXPECT_FALSE(pg::parse_url("postgres://u@h:/d").has_value());
    EXPECT_FALSE(pg::parse_url("postgres://u:p%zz@h/d").has_value());
    EXPECT_FALSE(pg::parse_url("postgres://u:p%4@h/d").has_value());
    EXPECT_FALSE(pg::parse_url("postgres://u@[::1/d").has_value());
}

TEST(PgMessages, SyncAndExecuteAreTheDocumentedBytes)
{
    Str out;
    pg::append_sync(out);
    EXPECT_EQ(out, bytes({'S', 0, 0, 0, 4}));
    out = Str();
    pg::append_execute(out);
    EXPECT_EQ(out, bytes({'E', 0, 0, 0, 9, 0, 0, 0, 0, 0}));
}

TEST(PgMessages, StartupCarriesVersion3AndTheSettings)
{
    const tuning::Setting settings[] = {{"jit", "off"}};
    Str out;
    pg::append_startup(out, "feed", "db", settings, 1);
    Str expected = bytes({0, 3, 0, 0});
    expected = cat(expected, "user", '\0', "feed", '\0', "database", '\0', "db", '\0', "jit", '\0', "off", '\0');
    expected.push_back('\0');
    Str with_length;
    be32(with_length, static_cast<uint32_t>(expected.size() + 4));
    with_length.append(expected.data(), expected.size());
    EXPECT_EQ(out, with_length);
}

TEST(PgMessages, ParseListsTheParameterTypes)
{
    const uint32_t types[] = {20, 25};
    Str out;
    pg::append_parse(out, "s", "SELECT $1, $2", types, 2);
    Str body = cat("s", '\0', "SELECT $1, $2", '\0');
    be16(body, 2);
    be32(body, 20);
    be32(body, 25);
    Str expected = cat('P');
    be32(expected, static_cast<uint32_t>(body.size() + 4));
    expected.append(body.data(), body.size());
    EXPECT_EQ(out, expected);
}

TEST(PgMessages, BindIsBinaryAllTheWayAndHandlesNull)
{
    const Str id = int_be(7, 8);
    const pg::Value params[] = {{id.data(), 8}, {"hi", 2}, {nullptr, -1}};
    Str out;
    pg::append_bind(out, "get", params, 3);

    Str body = cat('\0', "get", '\0');
    be16(body, 1);
    be16(body, 1);
    be16(body, 3);
    be32(body, 8);
    body.append(id.data(), 8);
    be32(body, 2);
    body.append("hi", 2);
    be32(body, 0xFFFFFFFFu);
    be16(body, 1);
    be16(body, 1);
    Str expected = cat('B');
    be32(expected, static_cast<uint32_t>(body.size() + 4));
    expected.append(body.data(), body.size());
    EXPECT_EQ(out, expected);
}

TEST(PgMessages, PasswordAndSaslMessages)
{
    Str out;
    pg::append_password(out, "pw");
    EXPECT_EQ(out, cat('p', bytes({0, 0, 0, 7}), "pw", '\0'));
    out = Str();
    pg::append_sasl_initial(out, "SCRAM-SHA-256", "n,,x");
    Str body = cat("SCRAM-SHA-256", '\0');
    be32(body, 4);
    body.append("n,,x", 4);
    Str expected = cat('p');
    be32(expected, static_cast<uint32_t>(body.size() + 4));
    expected.append(body.data(), body.size());
    EXPECT_EQ(out, expected);
    out = Str();
    pg::append_sasl_response(out, "abc");
    EXPECT_EQ(out, cat('p', bytes({0, 0, 0, 7}), "abc"));
}

TEST(PgFrames, ReadsWholeFramesAndWaitsForPartialOnes)
{
    Str wire = cat('Z', bytes({0, 0, 0, 5}), 'I');
    wire = cat(wire, '1', bytes({0, 0, 0, 4}));
    size_t off = 0;
    pg::Frame f;
    ASSERT_EQ(pg::next_frame(view(wire), off, f), pg::FrameStatus::Ok);
    EXPECT_EQ(f.type, 'Z');
    EXPECT_EQ(Str(f.body), "I");
    ASSERT_EQ(pg::next_frame(view(wire), off, f), pg::FrameStatus::Ok);
    EXPECT_EQ(f.type, '1');
    EXPECT_TRUE(f.body.empty());
    EXPECT_EQ(off, wire.size());
    EXPECT_EQ(pg::next_frame(view(wire), off, f), pg::FrameStatus::Incomplete);

    for (size_t cut = 0; cut < 6; cut++)
    {
        const Str partial(wire.data(), cut);
        size_t o = 0;
        EXPECT_EQ(pg::next_frame(view(partial), o, f), pg::FrameStatus::Incomplete) << cut;
        EXPECT_EQ(o, 0u);
    }
}

TEST(PgFrames, ImpossibleLengthsAreErrors)
{
    size_t off = 0;
    pg::Frame f;
    Str tiny = cat('Z', bytes({0, 0, 0, 3}));
    EXPECT_EQ(pg::next_frame(view(tiny), off, f), pg::FrameStatus::Bad);
    Str huge = cat('D', bytes({0x7f, 0xff, 0xff, 0xff}));
    EXPECT_EQ(pg::next_frame(view(huge), off, f), pg::FrameStatus::Bad);
}

TEST(PgFrames, ErrorResponseFields)
{
    const Str body = cat('S', "ERROR", '\0', 'C', "23503", '\0', 'M', "no such post", '\0', '\0');
    const pg::ErrorInfo e = pg::parse_error(view(body));
    EXPECT_EQ(e.sqlstate, "23503");
    EXPECT_EQ(e.message, "no such post");
    EXPECT_TRUE(pg::parse_error("").sqlstate.empty());
    EXPECT_TRUE(pg::parse_error(view(Str("Cabc"))).sqlstate.empty())
        << "an unterminated field is ignored, not read past";
}

TEST(PgFrames, CommandTagRowCount)
{
    EXPECT_EQ(pg::command_rows(view(cat("INSERT 0 1", '\0'))), 1);
    EXPECT_EQ(pg::command_rows("INSERT 0 0"), 0);
    EXPECT_EQ(pg::command_rows("SELECT 20"), 20);
    EXPECT_EQ(pg::command_rows("BEGIN"), -1);
    EXPECT_EQ(pg::command_rows("7"), -1);
    EXPECT_EQ(pg::command_rows(""), -1);
}

TEST(PgRows, SplitsColumnsAndNull)
{
    const Str row = data_row({Str("ab"), Str(""), Str("xyz")});
    View c[3];
    ASSERT_TRUE(pg::split_row(view(row), c, 3));
    EXPECT_EQ(Str(c[0]), "ab");
    EXPECT_TRUE(c[1].empty());
    EXPECT_NE(c[1].data(), nullptr) << "an empty value is not NULL";
    EXPECT_EQ(Str(c[2]), "xyz");

    Str with_null;
    be16(with_null, 1);
    be32(with_null, 0xFFFFFFFFu);
    View n[1];
    ASSERT_TRUE(pg::split_row(view(with_null), n, 1));
    EXPECT_EQ(n[0].data(), nullptr);
}

TEST(PgRows, RejectsWrongCountsAndTruncation)
{
    const Str row = data_row({Str("ab"), Str("cd")});
    View c[3];
    EXPECT_FALSE(pg::split_row(view(row), c, 3));
    EXPECT_FALSE(pg::split_row(view(Str(row.data(), row.size() - 1)), c, 2));
    EXPECT_FALSE(pg::split_row(view(cat(row, 'x')), c, 2)) << "trailing bytes";
    EXPECT_FALSE(pg::split_row("", c, 2));
}

TEST(PgRows, IntegersOfEveryWidthAreSignExtended)
{
    int64_t v = 0;
    ASSERT_TRUE(pg::decode_int(view(int_be(-2, 2)), v));
    EXPECT_EQ(v, -2);
    ASSERT_TRUE(pg::decode_int(view(int_be(123456, 4)), v));
    EXPECT_EQ(v, 123456);
    ASSERT_TRUE(pg::decode_int(view(int_be(-5'000'000'000, 8)), v));
    EXPECT_EQ(v, -5'000'000'000);
    ASSERT_TRUE(pg::decode_int(view(int_be(INT64_MAX, 8)), v));
    EXPECT_EQ(v, INT64_MAX);
    EXPECT_FALSE(pg::decode_int(view(int_be(1, 3)), v));
    EXPECT_FALSE(pg::decode_int(View(), v));
}

TEST(PgTimestamp, EpochAndNeighbours)
{
    Str s;
    pg::append_timestamp_text(s, 0);
    EXPECT_EQ(s, "2000-01-01 00:00:00.000000+00");
    s = Str();
    pg::append_timestamp_text(s, -1);
    EXPECT_EQ(s, "1999-12-31 23:59:59.999999+00");
    s = Str();
    pg::append_timestamp_text(s, micros_2000(8825, 12 * 3600 + 34 * 60 + 56, 789012));
    EXPECT_EQ(s, "2024-02-29 12:34:56.789012+00");
}

TEST(PgTimestamp, FeedsFormatCreatedAtUnchanged)
{
    Str s;
    pg::append_timestamp_text(s, micros_2000(9496, 23 * 3600 + 59 * 60 + 19, 409123));
    EXPECT_EQ(s, "2025-12-31 23:59:19.409123+00");
}

TEST(PgDecode, PostRow)
{
    const Str row =
        data_row({int_be(42, 8), Str("hello"), int_be(micros_2000(9496, 86399, 5), 8), Str("ann"), int_be(3, 4)});
    Post p;
    ASSERT_TRUE(pg::decode_post(view(row), p));
    EXPECT_EQ(p.id, 42);
    EXPECT_EQ(p.body, "hello");
    EXPECT_EQ(p.created_at, "2025-12-31 23:59:59.000005+00");
    EXPECT_EQ(p.author, "ann");
    EXPECT_EQ(p.like_count, 3);
}

TEST(PgDecode, PostRowRejectsNullAndWrongSizes)
{
    Post p;
    EXPECT_FALSE(pg::decode_post(view(data_row({int_be(1, 8), Str("b"), int_be(0, 4), Str("a"), int_be(0, 4)})), p))
        << "created_at must be eight bytes";
    Str null_body;
    be16(null_body, 5);
    be32(null_body, 8);
    null_body.append(int_be(1, 8).data(), 8);
    be32(null_body, 0xFFFFFFFFu);
    EXPECT_FALSE(pg::decode_post(view(null_body), p));
}

TEST(PgDecode, CreatedRow)
{
    const Str row = data_row({int_be(9, 8), int_be(micros_2000(0, 61, 0), 8)});
    NewPost n;
    ASSERT_TRUE(pg::decode_created(view(row), n));
    EXPECT_EQ(n.id, 9);
    EXPECT_EQ(n.created_at, "2000-01-01 00:01:01.000000+00");
    EXPECT_FALSE(pg::decode_created(view(data_row({int_be(9, 8)})), n));
}

using F = pg::Fate;

std::vector<F> fates(std::vector<DbKind> kinds, size_t error_at, const char *sqlstate = "XX000")
{
    pg::ErrorInfo e;
    e.sqlstate = String(sqlstate);
    std::vector<F> out(kinds.size());
    pg::batch_fates(kinds.data(), kinds.size(), error_at, &e, out.data());
    return out;
}

TEST(PgBatch, NoErrorDeliversEverythingWrittenOrNot)
{
    EXPECT_EQ(fates({DbKind::Like, DbKind::Feed, DbKind::Create}, 3),
              (std::vector<F>{F::Deliver, F::Deliver, F::Deliver}));
}

TEST(PgBatch, AfterAnErrorReadsBeforeItKeepTheirAnswersAndWritesAreReplayed)
{
    EXPECT_EQ(fates({DbKind::Feed, DbKind::Create, DbKind::Like, DbKind::Get}, 2),
              (std::vector<F>{F::Deliver, F::Replay, F::Replay, F::Replay}));
}

TEST(PgBatch, ADataOrConstraintErrorBelongsToItsQueryAlone)
{
    EXPECT_EQ(fates({DbKind::Like, DbKind::Like, DbKind::Like}, 1, "23503"),
              (std::vector<F>{F::Replay, F::Error, F::Replay}));
    EXPECT_EQ(fates({DbKind::Create, DbKind::Create}, 0, "22021"), (std::vector<F>{F::Error, F::Replay}));
}

TEST(PgBatch, OtherErrorsGiveTheFailingQueryASecondTryUnlessItRanAlone)
{
    EXPECT_EQ(fates({DbKind::Like, DbKind::Like}, 1, "40P01"), (std::vector<F>{F::Replay, F::Replay}));
    EXPECT_EQ(fates({DbKind::Like}, 0, "40P01"), (std::vector<F>{F::Error}));
}

TEST(PgBatch, NoErrorInfoStillTerminates)
{
    DbKind kinds[2] = {DbKind::Feed, DbKind::Get};
    F out[2];
    pg::batch_fates(kinds, 2, 0, nullptr, out);
    EXPECT_EQ(out[0], F::Replay);
    EXPECT_EQ(out[1], F::Replay);
}

TEST(PgScram, MatchesTheRfc7677Example)
{
    pg::Scram scram("user", "pencil", "rOprNGfwEbeRWgbNEkqO");
    EXPECT_EQ(scram.first_message(), "n,,n=user,r=rOprNGfwEbeRWgbNEkqO");

    const auto final_message =
        scram.final_message("r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096");
    ASSERT_TRUE(final_message.has_value()) << final_message.error().c_str();
    EXPECT_EQ(*final_message, "c=biws,r=rOprNGfwEbeRWgbNEkqO%hvYDpWUa2RaTCAfuxFIlj)hNlF$k0,"
                              "p=dHzbZapWIk4jUhN+Ute9ytag9zjfMHgsqmmiz7AndVQ=");

    EXPECT_TRUE(scram.verify("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
    EXPECT_FALSE(scram.verify("v=6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G5="));
    EXPECT_FALSE(scram.verify("6rriTRBi23WpRR/wtup+mMhUZUn/dB5nLTJRsjl95G4="));
}

TEST(PgScram, RefusesAServerNonceThatDoesNotStartWithOurs)
{
    pg::Scram scram("", "pw", "abcdef");
    EXPECT_EQ(scram.first_message(), "n,,n=,r=abcdef");
    EXPECT_FALSE(scram.final_message("r=zzzzzzzzzz,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096").has_value());
    EXPECT_FALSE(scram.final_message("r=abcdef,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=4096").has_value()) << "no server part";
}

TEST(PgScram, RefusesBadSaltsAndIterationCounts)
{
    pg::Scram scram("", "pw", "abc");
    EXPECT_FALSE(scram.final_message("r=abcdef,s=!!!,i=4096").has_value());
    EXPECT_FALSE(scram.final_message("r=abcdef,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=0").has_value());
    EXPECT_FALSE(scram.final_message("r=abcdef,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=x").has_value());
    EXPECT_FALSE(scram.final_message("r=abcdef,s=W22ZaJ0SNY7soEsUEjb6gQ==,i=99999999999").has_value());
    EXPECT_FALSE(scram.verify("v=anything")) << "never answered";
}

TEST(PgScram, RandomNoncesAreLongDistinctAndSafeInAMessage)
{
    const Str a = pg::random_nonce();
    const Str b = pg::random_nonce();
    EXPECT_EQ(a.size(), 24u);
    EXPECT_NE(a, b);
    for (const char c : a)
        EXPECT_TRUE(c != ',' && c >= '+' && c <= 'z');
}

} // namespace
} // namespace feed::test
