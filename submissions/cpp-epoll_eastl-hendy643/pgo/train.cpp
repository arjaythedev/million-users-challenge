// Training run for build.sh's profile-guided build: real parsing, auth, JSON and gzip code with canned database
// answers.
#include <mbedtls/md.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <string>

#include "api.hpp"
#include "http.hpp"
#include "response.hpp"

using namespace feed;

namespace
{
const std::string kSecret = "million-users-challenge";

std::string base64url(const unsigned char *data, const size_t n)
{
    static constexpr char kAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string out;
    for (size_t i = 0; i < n; i += 3)
    {
        const uint32_t v = (data[i] << 16) | (i + 1 < n ? data[i + 1] << 8 : 0) | (i + 2 < n ? data[i + 2] : 0);
        out += kAlphabet[v >> 18 & 63];
        out += kAlphabet[v >> 12 & 63];
        if (i + 1 < n)
            out += kAlphabet[v >> 6 & 63];
        if (i + 2 < n)
            out += kAlphabet[v & 63];
    }
    return out;
}

std::string token(const long sub, const std::string &username, const long exp)
{
    const std::string head = "{\"alg\":\"HS256\",\"typ\":\"JWT\"}";
    const std::string payload = "{\"sub\":\"" + std::to_string(sub) + "\",\"username\":\"" + username +
                                "\",\"exp\":" + std::to_string(exp) + "}";
    const auto enc = [](const std::string &s) {
        return base64url(reinterpret_cast<const unsigned char *>(s.data()), s.size());
    };
    const std::string signing = enc(head) + "." + enc(payload);
    unsigned char mac[32];
    mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                    reinterpret_cast<const unsigned char *>(kSecret.data()), kSecret.size(),
                    reinterpret_cast<const unsigned char *>(signing.data()), signing.size(), mac);
    return signing + "." + base64url(mac, sizeof mac);
}

std::string get(const std::string &path, const bool gzip)
{
    return "GET " + path + " HTTP/1.1\r\nHost: bench\r\nUser-Agent: mload\r\n" +
           (gzip ? "Accept-Encoding: gzip\r\n" : "") + "\r\n";
}

std::string post(const std::string &path, const std::string &bearer, const std::string &body)
{
    return "POST " + path + " HTTP/1.1\r\nHost: bench\r\nContent-Type: application/json\r\n" +
           (bearer.empty() ? "" : "Authorization: Bearer " + bearer + "\r\n") +
           "Content-Length: " + std::to_string(body.size()) + "\r\n\r\n" + body;
}

Post make_post(const int i)
{
    Post p;
    p.id = 100000 + i;
    std::string text = "Just shipped a new feature for the team, really happy with how it turned out " +
                       std::to_string(i) + "! #release #team";
    if (i % 7 == 0)
        text += " \"quoted\" and a back\\slash";
    if (i % 11 == 0)
        text += "\nsecond line\twith a tab";
    if (i % 13 == 0)
        text += " caf\xC3\xA9 \xE2\x9C\x93";
    p.body = String(text.c_str());
    p.created_at = String("2026-10-27 12:34:56.123456+00");
    p.author = String(("user_" + std::to_string(1000 + i % 500)).c_str());
    p.like_count = i % 40;
    return p;
}
} // namespace

int main(const int argc, char **argv)
{
    const long iterations = argc > 1 ? std::atol(argv[1]) : 300000;
    const long now = static_cast<long>(std::time(nullptr));
    const std::string bearer = token(4242, "alice", now + 3600);

    const std::string feed = get("/feed", true);
    const std::string feed_plain = get("/feed", false);
    const std::string one_post = get("/posts/123456", true);
    const std::string health = get("/health", false);
    const std::string create =
        post("/posts", bearer, "{\"body\":\"Hello from the training run, a post of ordinary length.\"}");
    const std::string like = post("/posts/123456/like", bearer, "");
    const std::string unauthorized = post("/posts", "", "{\"body\":\"x\"}");
    const std::string missing = get("/nowhere", false);
    const std::string bad_id = get("/posts/abc", false);

    eastl::vector<Post> page;
    for (int i = 0; i < 20; i++)
        page.push_back(make_post(i));

    uint64_t checksum = 0;
    String out;
    for (long i = 0; i < iterations; i++)
    {
        const long dice = i % 100;
        const std::string &raw = dice < 62   ? feed
                                 : dice < 66 ? feed_plain
                                 : dice < 84 ? one_post
                                 : dice < 88 ? health
                                 : dice < 92 ? create
                                 : dice < 96 ? like
                                 : dice < 97 ? unauthorized
                                 : dice < 98 ? missing
                                             : bad_id;

        const ParseResult parsed = parse_http_request(View(raw.data(), raw.size()));
        if (parsed.status != ParseStatus::Complete)
            continue;
        Plan plan = plan_request(View(kSecret.data(), kSecret.size()), now, parsed.request);

        ApiResponse response;
        if (plan.done)
        {
            response = std::move(plan.response);
        }
        else
        {
            DbReply reply;
            switch (plan.op.kind)
            {
                case DbKind::Feed:
                    reply.posts = page;
                    break;
                case DbKind::Get:
                    if (i % 9 != 0)
                        reply.posts.push_back(page[static_cast<size_t>(i) % page.size()]);
                    break;
                case DbKind::Create:
                    reply.created.id = 200000 + i;
                    reply.created.created_at = String("2026-10-27 12:34:56.654321+00");
                    break;
                case DbKind::Like:
                    reply.like = i % 5 == 0 ? LikeOutcome::AlreadyLiked : LikeOutcome::Created;
                    break;
                case DbKind::Ping:
                    break;
            }
            response = finish_request(plan.op, std::move(reply), 120);
        }
        out.clear();
        append_http_response(out, response.status, View(response.body), parsed.request.keep_alive,
                             parsed.request.accepts_gzip);
        checksum += out.size();
    }
    std::printf("trained on %ld requests (%llu bytes of responses)\n", iterations,
                static_cast<unsigned long long>(checksum));
    return 0;
}
