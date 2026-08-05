#ifndef APPLESS_LITE_SERVER_JSON_VALUE_H
#define APPLESS_LITE_SERVER_JSON_VALUE_H

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

namespace appless::lite_server {

class JsonValue {
public:
    enum class Type { Null, Boolean, Number, String, Array, Object };

    using Array = std::vector<JsonValue>;
    using Object = std::unordered_map<std::string, JsonValue>;

    JsonValue() = default;

    static JsonValue Null();
    static JsonValue Boolean(bool value);
    static JsonValue Number(double value);
    static JsonValue String(std::string value);
    static JsonValue ArrayValue(Array value);
    static JsonValue ObjectValue(Object value);

    Type type() const;
    bool boolean() const;
    double number() const;
    const std::string &string() const;
    const Array &array() const;
    const Object &object() const;
    const JsonValue *Find(const std::string &key) const;

private:
    Type type_ = Type::Null;
    bool boolean_ = false;
    double number_ = 0;
    std::string string_;
    std::unique_ptr<Array> array_;
    std::unique_ptr<Object> object_;
};

bool ParseJson(const std::string &text, JsonValue *value, std::string *error);

}  // namespace appless::lite_server

#endif
