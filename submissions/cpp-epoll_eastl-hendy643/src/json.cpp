#include "json.hpp"

namespace feed
{
namespace
{

constexpr int kMaxDepth = 64;
constexpr size_t kMaxBodyCharacters = 500;

class Parser
{
  public:
    explicit Parser(const View text) :
        p_(text.data()),
        end_(text.data() + text.size())
    {
    }

    bool parse(JsonDocument &doc)
    {
        skip_space();
        bool ok;
        if (p_ < end_ && *p_ == '{')
        {
            doc.is_object = true;
            ok = parse_object(0, &doc.fields);
        }
        else
        {
            ok = parse_value(0);
        }
        skip_space();
        return ok && p_ == end_;
    }

  private:
    const char *p_;
    const char *end_;

    void skip_space()
    {
        while (p_ < end_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r'))
            ++p_;
    }

    bool consume(const char *literal)
    {
        const char *q = p_;
        for (; *literal; ++literal, ++q)
        {
            if (q >= end_ || *q != *literal)
                return false;
        }
        p_ = q;
        return true;
    }

    static int hex_value(const char c)
    {
        if (c >= '0' && c <= '9')
            return c - '0';
        if (c >= 'a' && c <= 'f')
            return c - 'a' + 10;
        if (c >= 'A' && c <= 'F')
            return c - 'A' + 10;
        return -1;
    }

    bool parse_hex4(unsigned &out)
    {
        if (end_ - p_ < 4)
            return false;
        out = 0;
        for (int i = 0; i < 4; ++i)
        {
            const int v = hex_value(p_[i]);
            if (v < 0)
                return false;
            out = out * 16 + static_cast<unsigned>(v);
        }
        p_ += 4;
        return true;
    }

    static void append_utf8(String &out, const unsigned cp)
    {
        if (cp < 0x80)
        {
            out.push_back(static_cast<char>(cp));
        }
        else if (cp < 0x800)
        {
            out.push_back(static_cast<char>(0xC0 | (cp >> 6)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else if (cp < 0x10000)
        {
            out.push_back(static_cast<char>(0xE0 | (cp >> 12)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
        else
        {
            out.push_back(static_cast<char>(0xF0 | (cp >> 18)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 12) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | ((cp >> 6) & 0x3F)));
            out.push_back(static_cast<char>(0x80 | (cp & 0x3F)));
        }
    }

    bool parse_string(String *out)
    {
        ++p_;
        while (p_ < end_)
        {
            const auto c = static_cast<unsigned char>(*p_++);
            if (c == '"')
                return true;
            if (c < 0x20)
                return false;
            if (c != '\\')
            {
                if (out)
                    out->push_back(static_cast<char>(c));
                continue;
            }
            if (p_ >= end_)
                return false;
            const char e = *p_++;
            char simple = 0;
            switch (e)
            {
                case '"':
                    simple = '"';
                    break;
                case '\\':
                    simple = '\\';
                    break;
                case '/':
                    simple = '/';
                    break;
                case 'b':
                    simple = '\b';
                    break;
                case 'f':
                    simple = '\f';
                    break;
                case 'n':
                    simple = '\n';
                    break;
                case 'r':
                    simple = '\r';
                    break;
                case 't':
                    simple = '\t';
                    break;
                case 'u':
                    break;
                default:
                    return false;
            }
            if (e != 'u')
            {
                if (out)
                    out->push_back(simple);
                continue;
            }
            unsigned cp;
            if (!parse_hex4(cp))
                return false;
            if (cp >= 0xD800 && cp <= 0xDBFF)
            {
                if (end_ - p_ < 2 || p_[0] != '\\' || p_[1] != 'u')
                    return false;
                p_ += 2;
                unsigned low;
                if (!parse_hex4(low) || low < 0xDC00 || low > 0xDFFF)
                    return false;
                cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
            }
            else if (cp >= 0xDC00 && cp <= 0xDFFF)
            {
                return false;
            }
            if (out)
                append_utf8(*out, cp);
        }
        return false;
    }

    static bool is_digit(const char c)
    {
        return c >= '0' && c <= '9';
    }

    bool parse_digits()
    {
        if (p_ >= end_ || !is_digit(*p_))
            return false;
        while (p_ < end_ && is_digit(*p_))
            ++p_;
        return true;
    }

    bool parse_number()
    {
        if (p_ < end_ && *p_ == '-')
            ++p_;
        if (p_ >= end_)
            return false;
        if (*p_ == '0')
        {
            ++p_;
        }
        else if (!parse_digits())
        {
            return false;
        }
        if (p_ < end_ && *p_ == '.')
        {
            ++p_;
            if (!parse_digits())
                return false;
        }
        if (p_ < end_ && (*p_ == 'e' || *p_ == 'E'))
        {
            ++p_;
            if (p_ < end_ && (*p_ == '+' || *p_ == '-'))
                ++p_;
            if (!parse_digits())
                return false;
        }
        return true;
    }

    bool parse_array(const int depth)
    {
        ++p_;
        skip_space();
        if (p_ < end_ && *p_ == ']')
        {
            ++p_;
            return true;
        }
        for (;;)
        {
            if (!parse_value(depth + 1))
                return false;
            skip_space();
            if (p_ >= end_)
                return false;
            if (*p_ == ',')
            {
                ++p_;
                continue;
            }
            if (*p_ == ']')
            {
                ++p_;
                return true;
            }
            return false;
        }
    }

    bool parse_object(const int depth, eastl::vector<JsonField> *fields)
    {
        if (depth > kMaxDepth)
            return false;
        ++p_;
        skip_space();
        if (p_ < end_ && *p_ == '}')
        {
            ++p_;
            return true;
        }
        for (;;)
        {
            skip_space();
            if (p_ >= end_ || *p_ != '"')
                return false;
            JsonField field;
            field.type = JsonType::Other;
            if (!parse_string(&field.key))
                return false;
            skip_space();
            if (p_ >= end_ || *p_ != ':')
                return false;
            ++p_;
            skip_space();
            if (p_ >= end_)
                return false;
            if (*p_ == '"')
            {
                field.type = JsonType::String;
                if (!parse_string(&field.text))
                    return false;
            }
            else if (*p_ == '-' || is_digit(*p_))
            {
                const char *start = p_;
                if (!parse_number())
                    return false;
                field.type = JsonType::Number;
                field.text = String(start, static_cast<size_t>(p_ - start));
            }
            else if (!parse_value(depth + 1))
            {
                return false;
            }
            if (fields)
                fields->push_back(eastl::move(field));
            skip_space();
            if (p_ >= end_)
                return false;
            if (*p_ == ',')
            {
                ++p_;
                continue;
            }
            if (*p_ == '}')
            {
                ++p_;
                return true;
            }
            return false;
        }
    }

    bool parse_value(const int depth)
    {
        if (depth > kMaxDepth)
            return false;
        skip_space();
        if (p_ >= end_)
            return false;
        switch (*p_)
        {
            case '"':
                return parse_string(nullptr);
            case '{':
                return parse_object(depth, nullptr);
            case '[':
                return parse_array(depth);
            case 't':
                return consume("true");
            case 'f':
                return consume("false");
            case 'n':
                return consume("null");
            default:
                return parse_number();
        }
    }
};

bool is_space(const char c)
{
    return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v';
}

} // namespace

const JsonField *JsonDocument::find(const View key) const
{
    for (size_t i = fields.size(); i-- > 0;)
    {
        if (View(fields[i].key) == key)
            return &fields[i];
    }
    return nullptr;
}

bool parse_json(const View text, JsonDocument &doc)
{
    doc = JsonDocument();
    return Parser(text).parse(doc);
}

std::expected<String, BodyError> parse_create_post_body(const View raw)
{
    JsonDocument doc;
    if (!parse_json(raw, doc))
        return std::unexpected(BodyError::MalformedJson);

    const JsonField *field = doc.is_object ? doc.find("body") : nullptr;
    if (!field || field->type != JsonType::String)
        return std::unexpected(BodyError::BodyRequired);

    View text(field->text);
    while (!text.empty() && is_space(text.front()))
        text.remove_prefix(1);
    while (!text.empty() && is_space(text.back()))
        text.remove_suffix(1);
    if (text.empty())
        return std::unexpected(BodyError::BodyRequired);

    size_t characters = 0;
    const char *bytes = text.data();
    const size_t size = text.size();
    for (size_t i = 0; i < size; i++)
    {
        characters += (static_cast<unsigned char>(bytes[i]) & 0xC0) != 0x80;
    }
    if (characters > kMaxBodyCharacters)
        return std::unexpected(BodyError::BodyTooLong);

    return String(text);
}

} // namespace feed
