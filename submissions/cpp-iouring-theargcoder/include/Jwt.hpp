#pragma once

#include <openssl/crypto.h>
#include <openssl/evp.h>

#include <array>
#include <charconv>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <system_error>
#include <utility>

namespace HttpServer::jwt_auth
{
  inline constexpr size_t MAX_TOKEN_SIZE = 4096;
  inline constexpr size_t MAX_HEADER_JSON_SIZE = 256;
  inline constexpr size_t MAX_PAYLOAD_JSON_SIZE = MAX_TOKEN_SIZE;
  inline constexpr size_t MAX_USERNAME_SIZE = 32;

  struct Identity
  {
    uint64_t user_id{ 0 };

    // Owns the decoded username. Never points into Verifier's stack buffers.
    std::array<char, MAX_USERNAME_SIZE> username{};
    size_t username_size{ 0 };

    [[nodiscard]] std::string_view username_view() const noexcept
    {
      return { username.data(), username_size };
    }
  };

  enum class State : uint8_t
  {
    MISSING,
    AUTHENTICATED,
    INVALID,
    INVALID_PAYLOAD
  };

  struct Result
  {
    State state{ State::MISSING };
    Identity identity{};

    [[nodiscard]] constexpr bool accepted(bool auth_required) const noexcept
    {
      if(state == State::INVALID || state == State::INVALID_PAYLOAD)
      {
        return false;
      }

      return !auth_required || state == State::AUTHENTICATED;
    }
  };

  [[nodiscard]] inline int64_t unix_time_seconds() noexcept
  {
    return std::chrono::duration_cast<std::chrono::seconds>(std::chrono::system_clock::now().time_since_epoch()).count();
  }

  namespace detail
  {
    [[nodiscard]] constexpr std::array<int8_t, 256> make_b64url_table() noexcept
    {
      std::array<int8_t, 256> table{};

      for(auto &value : table)
      {
        value = -1;
      }

      for(int i = 0; i < 26; ++i)
      {
        table[static_cast<uint8_t>('A' + i)] = static_cast<int8_t>(i);
        table[static_cast<uint8_t>('a' + i)] = static_cast<int8_t>(26 + i);
      }

      for(int i = 0; i < 10; ++i)
      {
        table[static_cast<uint8_t>('0' + i)] = static_cast<int8_t>(52 + i);
      }

      table[static_cast<uint8_t>('-')] = 62;
      table[static_cast<uint8_t>('_')] = 63;

      return table;
    }

    inline constexpr auto b64url_table = make_b64url_table();

    // Strict, unpadded Base64URL decoder.
    // Rejects invalid alphabet, impossible lengths, overflow, padding,
    // and nonzero unused trailing bits.
    [[nodiscard]] inline bool decode_b64url(std::string_view input, std::span<uint8_t> output, size_t &output_size) noexcept
    {
      output_size = 0;

      if(input.empty() || input.size() % 4 == 1)
      {
        return false;
      }

      uint32_t accumulator = 0;
      unsigned bits = 0;

      for(const char raw : input)
      {
        const auto c = static_cast<uint8_t>(raw);
        const int8_t value = b64url_table[c];

        if(value < 0)
        {
          return false;
        }

        accumulator = (accumulator << 6U) | static_cast<uint32_t>(value);
        bits += 6U;

        if(bits >= 8U)
        {
          bits -= 8U;

          if(output_size == output.size())
          {
            return false;
          }

          output[output_size++] = static_cast<uint8_t>((accumulator >> bits) & 0xFFU);

          accumulator = bits == 0U ? 0U : (accumulator & ((1U << bits) - 1U));
        }
      }

      return accumulator == 0U;
    }

    enum class ClaimsState : uint8_t
    {
      VALID,
      MALFORMED,
      INVALID_IDENTITY
    };

    class JsonObjectScanner
    {
    private:
      std::string_view input;
      size_t pos{ 0 };
      bool alg_ok{ false };

    public:
      explicit JsonObjectScanner(std::string_view _input) noexcept : input(_input)
      {
      }

      [[nodiscard]] bool parse_header() noexcept
      {
        if(!begin_object())
        {
          return false;
        }

        bool found_alg = false;

        for(;;)
        {
          skip_ws();

          if(consume('}'))
          {
            return found_alg && alg_ok && at_end();
          }

          std::string_view key;

          if(!parse_plain_string(key))
          {
            return false;
          }

          skip_ws();

          if(!consume(':'))
          {
            return false;
          }

          skip_ws();

          if(key == "alg")
          {
            if(found_alg)
            {
              return false;
            }

            found_alg = true;
            std::string_view value;

            if(!parse_plain_string(value))
            {
              return false;
            }

            alg_ok = (value == "HS256");
          }
          else if(key == "crit" || key == "b64")
          {
            // Do not silently accept JWS extensions that change
            // the signing semantics.
            return false;
          }
          else
          {
            if(!skip_value(0))
            {
              return false;
            }
          }

          skip_ws();

          if(consume('}'))
          {
            return found_alg && alg_ok && at_end();
          }

          if(!consume(','))
          {
            return false;
          }

          skip_ws();

          if(pos < input.size() && input[pos] == '}')
          {
            return false;
          }
        }
      }

      // Extracts:
      //   sub      -> positive uint64 integer encoded as a JSON string
      //   username -> nonempty JSON string, decoded into owned storage
      //   exp      -> nonnegative integer
      //
      // Malformed JSON is distinct from an invalid identity.
      [[nodiscard]] ClaimsState parse_claims(Identity &identity, int64_t &expiry) noexcept
      {
        identity = {};
        expiry = 0;

        if(!begin_object())
        {
          return ClaimsState::MALFORMED;
        }

        bool found_sub = false;
        bool found_username = false;
        bool found_exp = false;
        bool invalid_identity = false;

        for(;;)
        {
          skip_ws();

          if(consume('}'))
          {
            break;
          }

          std::string_view key;

          if(!parse_plain_string(key))
          {
            return ClaimsState::MALFORMED;
          }

          skip_ws();

          if(!consume(':'))
          {
            return ClaimsState::MALFORMED;
          }

          skip_ws();

          if(key == "sub")
          {
            if(found_sub)
            {
              invalid_identity = true;

              if(!skip_value(0))
              {
                return ClaimsState::MALFORMED;
              }
            }
            else
            {
              found_sub = true;

              if(pos >= input.size() || input[pos] != '"')
              {
                if(!skip_value(0))
                {
                  return ClaimsState::MALFORMED;
                }

                invalid_identity = true;
              }
              else
              {
                std::array<char, MAX_USERNAME_SIZE> subject_buffer{};
                size_t subject_size = 0;

                if(!parse_json_string(std::span<char>{ subject_buffer }, subject_size))
                {
                  return ClaimsState::MALFORMED;
                }

                const std::string_view subject_text{ subject_buffer.data(), subject_size };

                bool digits_only = !subject_text.empty();

                for(const char raw : subject_text)
                {
                  const auto c = static_cast<uint8_t>(raw);

                  if(c < static_cast<uint8_t>('0') || c > static_cast<uint8_t>('9'))
                  {
                    digits_only = false;
                    break;
                  }
                }

                uint64_t subject = 0;

                if(digits_only)
                {
                  const auto [ptr, ec] = std::from_chars(subject_text.data(), subject_text.data() + subject_text.size(), subject);

                  if(ec != std::errc{} || ptr != subject_text.data() + subject_text.size() || subject == 0)
                  {
                    digits_only = false;
                  }
                }

                if(!digits_only)
                {
                  invalid_identity = true;
                }
                else
                {
                  identity.user_id = subject;
                }
              }
            }
          }
          else if(key == "username")
          {
            if(found_username)
            {
              invalid_identity = true;

              if(!skip_value(0))
              {
                return ClaimsState::MALFORMED;
              }
            }
            else
            {
              found_username = true;

              if(pos >= input.size() || input[pos] != '"')
              {
                if(!skip_value(0))
                {
                  return ClaimsState::MALFORMED;
                }

                invalid_identity = true;
              }
              else
              {
                if(!parse_json_string(std::span<char>{ identity.username }, identity.username_size))
                {
                  return ClaimsState::MALFORMED;
                }

                if(identity.username_size == 0)
                {
                  invalid_identity = true;
                }
              }
            }
          }
          else if(key == "exp")
          {
            if(found_exp)
            {
              return ClaimsState::MALFORMED;
            }

            found_exp = true;

            if(!parse_nonnegative_integer(expiry))
            {
              return ClaimsState::MALFORMED;
            }
          }
          else
          {
            if(!skip_value(0))
            {
              return ClaimsState::MALFORMED;
            }
          }

          skip_ws();

          if(consume('}'))
          {
            break;
          }

          if(!consume(','))
          {
            return ClaimsState::MALFORMED;
          }

          skip_ws();

          if(pos < input.size() && input[pos] == '}')
          {
            return ClaimsState::MALFORMED;
          }
        }

        skip_ws();

        if(pos != input.size() || !found_exp)
        {
          return ClaimsState::MALFORMED;
        }

        if(!found_sub || !found_username)
        {
          invalid_identity = true;
        }

        return invalid_identity ? ClaimsState::INVALID_IDENTITY : ClaimsState::VALID;
      }

    private:
      [[nodiscard]] bool at_end() noexcept
      {
        skip_ws();
        return pos == input.size();
      }

      void skip_ws() noexcept
      {
        while(pos < input.size())
        {
          const char c = input[pos];

          if(c == ' ' || c == '\t' || c == '\r' || c == '\n')
          {
            ++pos;
          }
          else
          {
            break;
          }
        }
      }

      [[nodiscard]] bool consume(char c) noexcept
      {
        if(pos < input.size() && input[pos] == c)
        {
          ++pos;
          return true;
        }

        return false;
      }

      [[nodiscard]] bool begin_object() noexcept
      {
        skip_ws();
        return consume('{');
      }

      // Used for JSON property names and the JWT alg value.
      // Deliberately rejects escaped spellings of property names.
      [[nodiscard]] bool parse_plain_string(std::string_view &value) noexcept
      {
        if(!consume('"'))
        {
          return false;
        }

        const size_t start = pos;

        while(pos < input.size())
        {
          const uint8_t c = static_cast<uint8_t>(input[pos]);

          if(c == '"')
          {
            value = input.substr(start, pos - start);
            ++pos;
            return true;
          }

          if(c == '\\' || c < 0x20U)
          {
            return false;
          }

          ++pos;
        }

        return false;
      }

      // Parses a JSON string and writes its decoded UTF-8 representation
      // into caller-owned storage.
      [[nodiscard]] bool parse_json_string(std::span<char> output, size_t &written) noexcept
      {
        written = 0;

        if(!consume('"'))
        {
          return false;
        }

        const auto emit = [&](uint8_t byte) noexcept -> bool
        {
          if(written >= output.size())
          {
            return false;
          }

          output[written++] = static_cast<char>(byte);
          return true;
        };

        const auto emit_codepoint = [&](uint32_t cp) noexcept -> bool
        {
          if(cp <= 0x7FU)
          {
            return emit(static_cast<uint8_t>(cp));
          }

          if(cp <= 0x7FFU)
          {
            return emit(static_cast<uint8_t>(0xC0U | (cp >> 6U))) && emit(static_cast<uint8_t>(0x80U | (cp & 0x3FU)));
          }

          if(cp <= 0xFFFFU)
          {
            return emit(static_cast<uint8_t>(0xE0U | (cp >> 12U))) && emit(static_cast<uint8_t>(0x80U | ((cp >> 6U) & 0x3FU))) && emit(static_cast<uint8_t>(0x80U | (cp & 0x3FU)));
          }

          return emit(static_cast<uint8_t>(0xF0U | (cp >> 18U))) && emit(static_cast<uint8_t>(0x80U | ((cp >> 12U) & 0x3FU)))
                 && emit(static_cast<uint8_t>(0x80U | ((cp >> 6U) & 0x3FU))) && emit(static_cast<uint8_t>(0x80U | (cp & 0x3FU)));
        };

        const auto read_hex4 = [&](uint32_t &value) noexcept -> bool
        {
          if(input.size() - pos < 4)
          {
            return false;
          }

          value = 0;

          for(unsigned i = 0; i < 4; ++i)
          {
            const int8_t c = static_cast<int8_t>(input[pos++]);
            unsigned digit = 0;

            if(c >= '0' && c <= '9')
            {
              digit = static_cast<unsigned>(static_cast<int>(c) - '0');
            }
            else if(c >= 'a' && c <= 'f')
            {
              digit = static_cast<unsigned>(c) - static_cast<unsigned>('a') + 10U;
            }
            else if(c >= 'A' && c <= 'F')
            {
              digit = static_cast<unsigned>(c) - static_cast<unsigned>('A') + 10U;
            }
            else
            {
              return false;
            }

            value = (value << 4U) | digit;
          }

          return true;
        };

        while(pos < input.size())
        {
          const uint8_t c = static_cast<uint8_t>(input[pos++]);

          if(c == '"')
          {
            return true;
          }

          if(c < 0x20U)
          {
            return false;
          }

          if(c != '\\')
          {
            if(!emit(c))
            {
              return false;
            }

            continue;
          }

          if(pos >= input.size())
          {
            return false;
          }

          const char escaped = input[pos++];

          switch(escaped)
          {
          case '"':
            if(!emit('"'))
            {
              return false;
            }
            break;

          case '\\':
            if(!emit('\\'))
            {
              return false;
            }
            break;

          case '/':
            if(!emit('/'))
            {
              return false;
            }
            break;

          case 'b':
            if(!emit('\b'))
            {
              return false;
            }
            break;

          case 'f':
            if(!emit('\f'))
            {
              return false;
            }
            break;

          case 'n':
            if(!emit('\n'))
            {
              return false;
            }
            break;

          case 'r':
            if(!emit('\r'))
            {
              return false;
            }
            break;

          case 't':
            if(!emit('\t'))
            {
              return false;
            }
            break;

          case 'u':
          {
            uint32_t cp = 0;

            if(!read_hex4(cp))
            {
              return false;
            }

            // Decode UTF-16 surrogate pairs.
            if(cp >= 0xD800U && cp <= 0xDBFFU)
            {
              if(input.size() - pos < 6 || input[pos] != '\\' || input[pos + 1] != 'u')
              {
                return false;
              }

              pos += 2;
              uint32_t low = 0;

              if(!read_hex4(low) || low < 0xDC00U || low > 0xDFFFU)
              {
                return false;
              }

              cp = 0x10000U + ((cp - 0xD800U) << 10U) + (low - 0xDC00U);
            }
            else if(cp >= 0xDC00U && cp <= 0xDFFFU)
            {
              return false;
            }

            if(!emit_codepoint(cp))
            {
              return false;
            }

            break;
          }

          default:
            return false;
          }
        }

        return false;
      }

      [[nodiscard]] bool parse_nonnegative_integer(int64_t &value) noexcept
      {
        const size_t start = pos;

        if(pos >= input.size() || input[pos] < '0' || input[pos] > '9')
        {
          return false;
        }

        if(input[pos] == '0')
        {
          ++pos;

          // Leading-zero JSON numbers are invalid.
          if(pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
          {
            return false;
          }
        }
        else
        {
          for(; pos < input.size() && input[pos] >= '0' && input[pos] <= '9'; ++pos)
          {
          }
        }

        const auto [ptr, ec] = std::from_chars(input.data() + start, input.data() + pos, value);

        return ec == std::errc{} && ptr == input.data() + pos;
      }

      [[nodiscard]] bool skip_string() noexcept
      {
        if(!consume('"'))
        {
          return false;
        }

        while(pos < input.size())
        {
          const auto c = static_cast<uint8_t>(input[pos++]);

          if(c == '"')
          {
            return true;
          }

          if(c < 0x20U)
          {
            return false;
          }

          if(c == '\\')
          {
            if(pos >= input.size())
            {
              return false;
            }

            const char escaped = input[pos++];

            if(escaped == '"' || escaped == '\\' || escaped == '/' || escaped == 'b' || escaped == 'f' || escaped == 'n' || escaped == 'r' || escaped == 't')
            {
              continue;
            }

            if(escaped != 'u' || input.size() - pos < 4)
            {
              return false;
            }

            for(unsigned i = 0; i < 4; ++i)
            {
              const uint8_t h = static_cast<uint8_t>(input[pos++]);

              if(!((h >= '0' && h <= '9') || (h >= 'a' && h <= 'f') || (h >= 'A' && h <= 'F')))
              {
                return false;
              }
            }
          }
        }

        return false;
      }

      [[nodiscard]] bool skip_number() noexcept
      {
        const size_t start = pos;

        if(consume('-') && pos == input.size())
        {
          return false;
        }

        if(consume('0'))
        {
          if(pos < input.size() && input[pos] >= '0' && input[pos] <= '9')
          {
            return false;
          }
        }
        else
        {
          if(pos >= input.size() || input[pos] < '1' || input[pos] > '9')
          {
            return false;
          }

          for(; pos < input.size() && input[pos] >= '0' && input[pos] <= '9'; ++pos)
          {
          }
        }

        if(consume('.'))
        {
          const size_t fraction_start = pos;

          for(; pos < input.size() && input[pos] >= '0' && input[pos] <= '9'; ++pos)
          {
          }

          if(fraction_start == pos)
          {
            return false;
          }
        }

        if(pos < input.size() && (input[pos] == 'e' || input[pos] == 'E'))
        {
          ++pos;

          if(pos < input.size() && (input[pos] == '+' || input[pos] == '-'))
          {
            ++pos;
          }

          const size_t exp_start = pos;

          for(; pos < input.size() && input[pos] >= '0' && input[pos] <= '9'; ++pos)
          {
          }

          if(exp_start == pos)
          {
            return false;
          }
        }

        return pos > start;
      }

      [[nodiscard]] bool skip_value(unsigned depth) noexcept
      {
        if(depth > 16)
        {
          return false;
        }

        skip_ws();

        if(pos >= input.size())
        {
          return false;
        }

        const char c = input[pos];

        if(c == '"')
        {
          return skip_string();
        }

        if(c == '{')
        {
          ++pos;
          skip_ws();

          if(consume('}'))
          {
            return true;
          }

          for(;;)
          {
            if(!skip_string())
            {
              return false;
            }

            skip_ws();

            if(!consume(':'))
            {
              return false;
            }

            if(!skip_value(depth + 1))
            {
              return false;
            }

            skip_ws();

            if(consume('}'))
            {
              return true;
            }

            if(!consume(','))
            {
              return false;
            }

            skip_ws();
          }
        }

        if(c == '[')
        {
          ++pos;
          skip_ws();

          if(consume(']'))
          {
            return true;
          }

          for(;;)
          {
            if(!skip_value(depth + 1))
            {
              return false;
            }

            skip_ws();

            if(consume(']'))
            {
              return true;
            }

            if(!consume(','))
            {
              return false;
            }
          }
        }

        if(c == 't' && input.substr(pos, 4) == "true")
        {
          pos += 4;
          return true;
        }

        if(c == 'f' && input.substr(pos, 5) == "false")
        {
          pos += 5;
          return true;
        }

        if(c == 'n' && input.substr(pos, 4) == "null")
        {
          pos += 4;
          return true;
        }

        return skip_number();
      }
    };

    // HMAC-SHA256 with precomputed inner and outer SHA-256 states.
    // Each Verifier owns one instance; the seed contexts are never mutated.
    // The working contexts are private to that Verifier and must not be used concurrently.
    // No EVP_MAC_init(), key reprocessing, or MAC fetch occurs in the request path.
    class HmacSha256 final
    {
    private:
      static constexpr size_t HMAC_BLOCK_SIZE = 64;
      static constexpr size_t HMAC_DIGEST_SIZE = 32;

      using MdContext = std::unique_ptr<EVP_MD_CTX, decltype(&EVP_MD_CTX_free)>;

      MdContext inner_seed_;
      MdContext outer_seed_;
      MdContext inner_work_;
      MdContext outer_work_;
      bool ready_{ false };

      [[nodiscard]] static bool initialize_pad(EVP_MD_CTX *ctx, const std::array<unsigned char, HMAC_BLOCK_SIZE> &pad) noexcept
      {
        return EVP_DigestInit_ex2(ctx, EVP_sha256(), nullptr) == 1 && EVP_DigestUpdate(ctx, pad.data(), pad.size()) == 1;
      }

      [[nodiscard]] bool initialize(std::string_view secret) noexcept
      {
        if(secret.empty() || !inner_seed_ || !outer_seed_ || !inner_work_ || !outer_work_)
        {
          return false;
        }

        std::array<unsigned char, HMAC_BLOCK_SIZE> key_block{};

        // HMAC keys longer than the SHA-256 block size are hashed first.
        if(secret.size() > HMAC_BLOCK_SIZE)
        {
          MdContext key_hash(EVP_MD_CTX_new(), &EVP_MD_CTX_free);
          unsigned int hash_size = 0;

          if(!key_hash || EVP_DigestInit_ex2(key_hash.get(), EVP_sha256(), nullptr) != 1 || EVP_DigestUpdate(key_hash.get(), secret.data(), secret.size()) != 1
             || EVP_DigestFinal_ex(key_hash.get(), key_block.data(), &hash_size) != 1 || hash_size != HMAC_DIGEST_SIZE)
          {
            OPENSSL_cleanse(key_block.data(), key_block.size());
            return false;
          }
        }
        else
        {
          std::memcpy(key_block.data(), secret.data(), secret.size());
        }

        std::array<unsigned char, HMAC_BLOCK_SIZE> ipad{};
        std::array<unsigned char, HMAC_BLOCK_SIZE> opad{};

        for(size_t i = 0; i < HMAC_BLOCK_SIZE; ++i)
        {
          ipad[i] = static_cast<unsigned char>(key_block[i] ^ 0x36U);
          opad[i] = static_cast<unsigned char>(key_block[i] ^ 0x5CU);
        }

        const bool initialized = initialize_pad(inner_seed_.get(), ipad) && initialize_pad(outer_seed_.get(), opad);

        OPENSSL_cleanse(key_block.data(), key_block.size());
        OPENSSL_cleanse(ipad.data(), ipad.size());
        OPENSSL_cleanse(opad.data(), opad.size());

        return initialized;
      }

      [[nodiscard]] bool calculate(std::string_view message, unsigned char *output) noexcept
      {
        std::array<unsigned char, HMAC_DIGEST_SIZE> inner_hash{};
        unsigned int inner_size = 0;
        unsigned int output_size = 0;

        // Restore the precomputed inner state and append this request's
        // exact JWT signing input.
        if(EVP_MD_CTX_copy_ex(inner_work_.get(), inner_seed_.get()) != 1)
        {
          return false;
        }

        if((!message.empty() && EVP_DigestUpdate(inner_work_.get(), message.data(), message.size()) != 1)
           || EVP_DigestFinal_ex(inner_work_.get(), inner_hash.data(), &inner_size) != 1 || inner_size != HMAC_DIGEST_SIZE)
        {
          OPENSSL_cleanse(inner_hash.data(), inner_hash.size());
          return false;
        }

        // Restore the precomputed outer state and hash the inner digest.
        if(EVP_MD_CTX_copy_ex(outer_work_.get(), outer_seed_.get()) != 1 || EVP_DigestUpdate(outer_work_.get(), inner_hash.data(), inner_hash.size()) != 1
           || EVP_DigestFinal_ex(outer_work_.get(), output, &output_size) != 1 || output_size != HMAC_DIGEST_SIZE)
        {
          OPENSSL_cleanse(inner_hash.data(), inner_hash.size());
          return false;
        }

        OPENSSL_cleanse(inner_hash.data(), inner_hash.size());
        return true;
      }

    public:
      explicit HmacSha256(std::string_view secret) noexcept
          : inner_seed_(EVP_MD_CTX_new(), &EVP_MD_CTX_free), outer_seed_(EVP_MD_CTX_new(), &EVP_MD_CTX_free), inner_work_(EVP_MD_CTX_new(), &EVP_MD_CTX_free),
            outer_work_(EVP_MD_CTX_new(), &EVP_MD_CTX_free)
      {
        ready_ = initialize(secret);
      }

      HmacSha256(const HmacSha256 &) = delete;
      HmacSha256 &operator=(const HmacSha256 &) = delete;

      HmacSha256(HmacSha256 &&other) noexcept
          : inner_seed_(std::move(other.inner_seed_)), outer_seed_(std::move(other.outer_seed_)), inner_work_(std::move(other.inner_work_)),
            outer_work_(std::move(other.outer_work_)), ready_(std::exchange(other.ready_, false))
      {
      }

      HmacSha256 &operator=(HmacSha256 &&other) noexcept
      {
        if(this != &other)
        {
          inner_seed_ = std::move(other.inner_seed_);
          outer_seed_ = std::move(other.outer_seed_);
          inner_work_ = std::move(other.inner_work_);
          outer_work_ = std::move(other.outer_work_);
          ready_ = std::exchange(other.ready_, false);
        }

        return *this;
      }

      [[nodiscard]] bool valid() const noexcept
      {
        return ready_;
      }

      [[nodiscard]] bool verify(std::string_view message, const unsigned char *expected, size_t expected_size) noexcept
      {
        if(!ready_ || expected == nullptr || expected_size != HMAC_DIGEST_SIZE)
        {
          return false;
        }

        std::array<unsigned char, HMAC_DIGEST_SIZE> actual{};

        if(!calculate(message, actual.data()))
        {
          OPENSSL_cleanse(actual.data(), actual.size());
          return false;
        }

        const bool matches = CRYPTO_memcmp(actual.data(), expected, HMAC_DIGEST_SIZE) == 0;

        OPENSSL_cleanse(actual.data(), actual.size());
        return matches;
      }
    };
  } // namespace detail

  // One verifier per worker/thread. Its mutable digest work contexts must
  // never be accessed concurrently.
  class Verifier
  {
  private:
    detail::HmacSha256 hmac;

  public:
    explicit Verifier(std::string_view secret) noexcept : hmac(secret)
    {
    }

    Verifier(const Verifier &) = delete;
    Verifier &operator=(const Verifier &) = delete;

    Verifier(Verifier &&other) noexcept : hmac(std::move(other.hmac))
    {
    }

    Verifier &operator=(Verifier &&other) noexcept
    {
      if(this != &other)
      {
        hmac = std::move(other.hmac);
      }

      return *this;
    }

    [[nodiscard]] bool is_ready() const noexcept
    {
      return hmac.valid();
    }

    // Verifies a compact JWT and extracts its authenticated identity.
    // Decoding, JSON scanning, and identity extraction use caller-owned
    // stack buffers; the secret-derived HMAC states are initialized once.
    [[nodiscard]] State verify(std::string_view token, Identity &identity, int64_t now = unix_time_seconds()) noexcept
    {
      identity = {};

      if(!is_ready() || token.empty() || token.size() > MAX_TOKEN_SIZE)
      {
        return State::INVALID;
      }

      const size_t dot1 = token.find('.');

      if(dot1 == std::string_view::npos)
      {
        return State::INVALID;
      }

      const size_t dot2 = token.find('.', dot1 + 1);

      if(dot2 == std::string_view::npos || token.find('.', dot2 + 1) != std::string_view::npos)
      {
        return State::INVALID;
      }

      const std::string_view header_b64 = token.substr(0, dot1);
      const std::string_view payload_b64 = token.substr(dot1 + 1, dot2 - dot1 - 1);
      const std::string_view signature_b64 = token.substr(dot2 + 1);

      if(header_b64.empty() || payload_b64.empty() || signature_b64.empty())
      {
        return State::INVALID;
      }

      std::array<uint8_t, MAX_HEADER_JSON_SIZE> header_buffer{};
      std::array<uint8_t, MAX_PAYLOAD_JSON_SIZE> payload_buffer{};
      std::array<uint8_t, 32> signature_buffer{};

      size_t header_size = 0;
      size_t payload_size = 0;
      size_t signature_size = 0;

      if(!detail::decode_b64url(header_b64, std::span<uint8_t>{ header_buffer }, header_size)
         || !detail::decode_b64url(payload_b64, std::span<uint8_t>{ payload_buffer }, payload_size)
         || !detail::decode_b64url(signature_b64, std::span<uint8_t>{ signature_buffer }, signature_size) || signature_size != signature_buffer.size())
      {
        return State::INVALID;
      }

      const std::string_view header_json{ reinterpret_cast<const char *>(header_buffer.data()), header_size };

      const std::string_view payload_json{ reinterpret_cast<const char *>(payload_buffer.data()), payload_size };

      detail::JsonObjectScanner header_parser(header_json);

      if(!header_parser.parse_header())
      {
        return State::INVALID;
      }

      // JWT signs the exact original encoded bytes, not re-serialized JSON.
      const std::string_view signing_input = token.substr(0, dot2);

      if(!hmac.verify(signing_input, signature_buffer.data(), signature_size))
      {
        return State::INVALID;
      }

      // Claims are parsed only after successful signature verification.
      detail::JsonObjectScanner claims_parser(payload_json);
      int64_t expiry = 0;

      const detail::ClaimsState claims_state = claims_parser.parse_claims(identity, expiry);

      if(claims_state == detail::ClaimsState::MALFORMED)
      {
        identity = {};
        return State::INVALID;
      }

      // Expiration is a JWT validity failure, even if another identity
      // claim is also invalid.
      if(expiry <= now)
      {
        identity = {};
        return State::INVALID;
      }

      if(claims_state == detail::ClaimsState::INVALID_IDENTITY)
      {
        identity = {};
        return State::INVALID_PAYLOAD;
      }

      return State::AUTHENTICATED;
    }
  };

  // Pass std::nullopt when Authorization is absent.
  // Otherwise pass the field value without "Authorization:". Expected scheme: Bearer <JWT>
  [[nodiscard]] inline Result verify_authorization(std::optional<std::string_view> authorization_value, Verifier &verifier, int64_t now = unix_time_seconds()) noexcept
  {
    if(!authorization_value.has_value())
    {
      return { .state = State::MISSING };
    }

    const std::string_view value = *authorization_value;
    size_t pos = 0;

    while(pos < value.size() && (value[pos] == ' ' || value[pos] == '\t'))
    {
      ++pos;
    }

    const size_t scheme_start = pos;

    while(pos < value.size() && value[pos] != ' ' && value[pos] != '\t')
    {
      ++pos;
    }

    const std::string_view scheme = value.substr(scheme_start, pos - scheme_start);
    constexpr std::string_view expected_scheme = "bearer";

    if(scheme.size() != expected_scheme.size())
    {
      return { .state = State::MISSING };
    }

    // Authentication scheme names are case-insensitive.
    for(size_t i = 0; i < scheme.size(); ++i)
    {
      char c = scheme[i];

      if(c >= 'A' && c <= 'Z')
      {
        c = static_cast<char>(c + ('a' - 'A'));
      }

      if(c != expected_scheme[i])
      {
        return { .state = State::MISSING };
      }
    }

    // A separator is required after the Bearer scheme.
    if(pos == value.size() || (value[pos] != ' ' && value[pos] != '\t'))
    {
      return { .state = State::MISSING };
    }

    while(pos < value.size() && (value[pos] == ' ' || value[pos] == '\t'))
    {
      ++pos;
    }

    if(pos == value.size())
    {
      return { .state = State::INVALID };
    }

    const std::string_view jwt = value.substr(pos);

    // Reject whitespace and control bytes inside or after credentials.
    for(const char raw : jwt)
    {
      const auto c = static_cast<uint8_t>(raw);

      if(c <= 0x20U || c == 0x7FU)
      {
        return { .state = State::INVALID };
      }
    }

    Identity identity{};
    const State state = verifier.verify(jwt, identity, now);

    return { .state = state, .identity = identity };
  }

} // namespace HttpServer::jwt_auth
