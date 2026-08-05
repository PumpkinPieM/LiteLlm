#include "json_value.h"

#include <cctype>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <string>
#include <utility>

namespace lite_llm::internal {
namespace {

constexpr int kMaximumJsonDepth = 64;

bool AppendCodePoint(std::uint32_t code_point, std::string *output)
{
    if (code_point <= 0x7F) {
        output->push_back(static_cast<char>(code_point));
    } else if (code_point <= 0x7FF) {
        output->push_back(static_cast<char>(0xC0 | (code_point >> 6U)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    } else if (code_point <= 0xFFFF) {
        output->push_back(static_cast<char>(0xE0 | (code_point >> 12U)));
        output->push_back(static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    } else if (code_point <= 0x10FFFF) {
        output->push_back(static_cast<char>(0xF0 | (code_point >> 18U)));
        output->push_back(static_cast<char>(0x80 | ((code_point >> 12U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80 | ((code_point >> 6U) & 0x3FU)));
        output->push_back(static_cast<char>(0x80 | (code_point & 0x3FU)));
    } else {
        return false;
    }
    return true;
}

int HexDigit(char value)
{
    if (value >= '0' && value <= '9') {
        return value - '0';
    }
    if (value >= 'a' && value <= 'f') {
        return value - 'a' + 10;
    }
    if (value >= 'A' && value <= 'F') {
        return value - 'A' + 10;
    }
    return -1;
}

class Parser final {
public:
    explicit Parser(const std::string &text) : text_(text) {}

    bool Parse(JsonValue *value, std::string *error)
    {
        SkipWhitespace();
        if (!ParseValue(0, value)) {
            *error = error_;
            return false;
        }
        SkipWhitespace();
        if (cursor_ != text_.size()) {
            Fail("unexpected characters after JSON value");
            *error = error_;
            return false;
        }
        return true;
    }

private:
    void SkipWhitespace()
    {
        while (cursor_ < text_.size()) {
            const char value = text_[cursor_];
            if (value != ' ' && value != '\t' && value != '\r' && value != '\n') {
                break;
            }
            ++cursor_;
        }
    }

    bool Fail(const std::string &message)
    {
        if (error_.empty()) {
            error_ = message + " at byte " + std::to_string(cursor_);
        }
        return false;
    }

    bool ConsumeLiteral(const char *literal)
    {
        const std::size_t length = std::char_traits<char>::length(literal);
        if (text_.compare(cursor_, length, literal) != 0) {
            return false;
        }
        cursor_ += length;
        return true;
    }

    bool ParseValue(int depth, JsonValue *value)
    {
        if (depth > kMaximumJsonDepth) {
            return Fail("JSON nesting exceeds limit");
        }
        SkipWhitespace();
        if (cursor_ >= text_.size()) {
            return Fail("expected JSON value");
        }
        const char current = text_[cursor_];
        if (current == 'n' && ConsumeLiteral("null")) {
            *value = JsonValue::Null();
            return true;
        }
        if (current == 't' && ConsumeLiteral("true")) {
            *value = JsonValue::Boolean(true);
            return true;
        }
        if (current == 'f' && ConsumeLiteral("false")) {
            *value = JsonValue::Boolean(false);
            return true;
        }
        if (current == '"') {
            std::string string;
            if (!ParseString(&string)) {
                return false;
            }
            *value = JsonValue::String(std::move(string));
            return true;
        }
        if (current == '[') {
            return ParseArray(depth + 1, value);
        }
        if (current == '{') {
            return ParseObject(depth + 1, value);
        }
        if (current == '-' || (current >= '0' && current <= '9')) {
            return ParseNumber(value);
        }
        return Fail("invalid JSON value");
    }

    bool ParseString(std::string *output)
    {
        if (cursor_ >= text_.size() || text_[cursor_] != '"') {
            return Fail("expected JSON string");
        }
        ++cursor_;
        std::string value;
        while (cursor_ < text_.size()) {
            const unsigned char current = static_cast<unsigned char>(text_[cursor_++]);
            if (current == '"') {
                *output = std::move(value);
                return true;
            }
            if (current < 0x20) {
                return Fail("unescaped control character in string");
            }
            if (current != '\\') {
                value.push_back(static_cast<char>(current));
                continue;
            }
            if (cursor_ >= text_.size()) {
                return Fail("unterminated string escape");
            }
            const char escaped = text_[cursor_++];
            switch (escaped) {
                case '"':
                case '\\':
                case '/':
                    value.push_back(escaped);
                    break;
                case 'b':
                    value.push_back('\b');
                    break;
                case 'f':
                    value.push_back('\f');
                    break;
                case 'n':
                    value.push_back('\n');
                    break;
                case 'r':
                    value.push_back('\r');
                    break;
                case 't':
                    value.push_back('\t');
                    break;
                case 'u': {
                    std::uint32_t code_point = 0;
                    if (!ParseHexQuad(&code_point)) {
                        return false;
                    }
                    if (code_point >= 0xD800 && code_point <= 0xDBFF) {
                        if (cursor_ + 2 > text_.size() || text_[cursor_] != '\\' || text_[cursor_ + 1] != 'u') {
                            return Fail("high surrogate has no low surrogate");
                        }
                        cursor_ += 2;
                        std::uint32_t low = 0;
                        if (!ParseHexQuad(&low)) {
                            return false;
                        }
                        if (low < 0xDC00 || low > 0xDFFF) {
                            return Fail("invalid low surrogate");
                        }
                        code_point = 0x10000 + ((code_point - 0xD800) << 10U) + (low - 0xDC00);
                    } else if (code_point >= 0xDC00 && code_point <= 0xDFFF) {
                        return Fail("unpaired low surrogate");
                    }
                    if (!AppendCodePoint(code_point, &value)) {
                        return Fail("invalid Unicode code point");
                    }
                    break;
                }
                default:
                    return Fail("invalid string escape");
            }
        }
        return Fail("unterminated JSON string");
    }

    bool ParseHexQuad(std::uint32_t *value)
    {
        if (cursor_ + 4 > text_.size()) {
            return Fail("incomplete Unicode escape");
        }
        std::uint32_t result = 0;
        for (int index = 0; index < 4; ++index) {
            const int digit = HexDigit(text_[cursor_++]);
            if (digit < 0) {
                return Fail("invalid Unicode escape");
            }
            result = (result << 4U) | static_cast<std::uint32_t>(digit);
        }
        *value = result;
        return true;
    }

    bool ParseArray(int depth, JsonValue *value)
    {
        ++cursor_;
        SkipWhitespace();
        JsonValue::Array array;
        if (cursor_ < text_.size() && text_[cursor_] == ']') {
            ++cursor_;
            *value = JsonValue::ArrayValue(std::move(array));
            return true;
        }
        while (cursor_ < text_.size()) {
            JsonValue item;
            if (!ParseValue(depth, &item)) {
                return false;
            }
            array.push_back(std::move(item));
            SkipWhitespace();
            if (cursor_ < text_.size() && text_[cursor_] == ']') {
                ++cursor_;
                *value = JsonValue::ArrayValue(std::move(array));
                return true;
            }
            if (cursor_ >= text_.size() || text_[cursor_] != ',') {
                return Fail("expected comma or closing bracket");
            }
            ++cursor_;
            SkipWhitespace();
        }
        return Fail("unterminated JSON array");
    }

    bool ParseObject(int depth, JsonValue *value)
    {
        ++cursor_;
        SkipWhitespace();
        JsonValue::Object object;
        if (cursor_ < text_.size() && text_[cursor_] == '}') {
            ++cursor_;
            *value = JsonValue::ObjectValue(std::move(object));
            return true;
        }
        while (cursor_ < text_.size()) {
            std::string key;
            if (!ParseString(&key)) {
                return false;
            }
            SkipWhitespace();
            if (cursor_ >= text_.size() || text_[cursor_] != ':') {
                return Fail("expected colon after object key");
            }
            ++cursor_;
            JsonValue item;
            if (!ParseValue(depth, &item)) {
                return false;
            }
            object[key] = std::move(item);
            SkipWhitespace();
            if (cursor_ < text_.size() && text_[cursor_] == '}') {
                ++cursor_;
                *value = JsonValue::ObjectValue(std::move(object));
                return true;
            }
            if (cursor_ >= text_.size() || text_[cursor_] != ',') {
                return Fail("expected comma or closing brace");
            }
            ++cursor_;
            SkipWhitespace();
        }
        return Fail("unterminated JSON object");
    }

    bool ParseNumber(JsonValue *value)
    {
        const std::size_t start = cursor_;
        if (text_[cursor_] == '-') {
            ++cursor_;
        }
        if (cursor_ >= text_.size()) {
            return Fail("incomplete JSON number");
        }
        if (text_[cursor_] == '0') {
            ++cursor_;
            if (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_])) != 0) {
                return Fail("leading zero in JSON number");
            }
        } else if (text_[cursor_] >= '1' && text_[cursor_] <= '9') {
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_])) != 0) {
                ++cursor_;
            }
        } else {
            return Fail("invalid JSON number");
        }
        if (cursor_ < text_.size() && text_[cursor_] == '.') {
            ++cursor_;
            const std::size_t fraction_start = cursor_;
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_])) != 0) {
                ++cursor_;
            }
            if (cursor_ == fraction_start) {
                return Fail("JSON fraction has no digits");
            }
        }
        if (cursor_ < text_.size() && (text_[cursor_] == 'e' || text_[cursor_] == 'E')) {
            ++cursor_;
            if (cursor_ < text_.size() && (text_[cursor_] == '+' || text_[cursor_] == '-')) {
                ++cursor_;
            }
            const std::size_t exponent_start = cursor_;
            while (cursor_ < text_.size() && std::isdigit(static_cast<unsigned char>(text_[cursor_])) != 0) {
                ++cursor_;
            }
            if (cursor_ == exponent_start) {
                return Fail("JSON exponent has no digits");
            }
        }
        const std::string raw = text_.substr(start, cursor_ - start);
        char *end = nullptr;
        const double number = std::strtod(raw.c_str(), &end);
        if (end == raw.c_str() || *end != '\0' || !std::isfinite(number)) {
            return Fail("invalid JSON number");
        }
        *value = JsonValue::Number(number);
        return true;
    }

    const std::string &text_;
    std::size_t cursor_ = 0;
    std::string error_;
};

}  // namespace

JsonValue JsonValue::Null()
{
    return JsonValue();
}

JsonValue JsonValue::Boolean(bool value)
{
    JsonValue result;
    result.type_ = Type::Boolean;
    result.boolean_ = value;
    return result;
}

JsonValue JsonValue::Number(double value)
{
    JsonValue result;
    result.type_ = Type::Number;
    result.number_ = value;
    return result;
}

JsonValue JsonValue::String(std::string value)
{
    JsonValue result;
    result.type_ = Type::String;
    result.string_ = std::move(value);
    return result;
}

JsonValue JsonValue::ArrayValue(Array value)
{
    JsonValue result;
    result.type_ = Type::Array;
    result.array_ = std::make_unique<Array>(std::move(value));
    return result;
}

JsonValue JsonValue::ObjectValue(Object value)
{
    JsonValue result;
    result.type_ = Type::Object;
    result.object_ = std::make_unique<Object>(std::move(value));
    return result;
}

JsonValue::Type JsonValue::type() const
{
    return type_;
}

bool JsonValue::boolean() const
{
    return boolean_;
}

double JsonValue::number() const
{
    return number_;
}

const std::string &JsonValue::string() const
{
    return string_;
}

const JsonValue::Array &JsonValue::array() const
{
    return *array_;
}

const JsonValue::Object &JsonValue::object() const
{
    return *object_;
}

const JsonValue *JsonValue::Find(const std::string &key) const
{
    if (type_ != Type::Object) {
        return nullptr;
    }
    const auto found = object_->find(key);
    return found == object_->end() ? nullptr : &found->second;
}

bool ParseJson(const std::string &text, JsonValue *value, std::string *error)
{
    if (value == nullptr || error == nullptr) {
        return false;
    }
    Parser parser(text);
    return parser.Parse(value, error);
}

}  // namespace lite_llm::internal
