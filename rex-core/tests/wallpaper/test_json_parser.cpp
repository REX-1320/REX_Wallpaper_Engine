#include <gtest/gtest.h>
#include "wallpaper/runtime/Json.hpp"

namespace rex::runtime {
namespace {

TEST(JsonTest, JS01_ParseValidObject) {
    auto result = Json::Parse(R"({"a":1,"b":"x"})");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsObject());

    const auto& obj = result->AsObject();
    EXPECT_EQ(obj.size(), 2u);
    EXPECT_EQ(obj.at("a").AsNumber(), 1.0);
    EXPECT_EQ(obj.at("b").AsString(), "x");
}

TEST(JsonTest, JS02_ParseArray) {
    auto result = Json::Parse(R"([1,2,3])");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsArray());

    const auto& arr = result->AsArray();
    EXPECT_EQ(arr.size(), 3u);
    EXPECT_EQ(arr[0].AsNumber(), 1.0);
    EXPECT_EQ(arr[1].AsNumber(), 2.0);
    EXPECT_EQ(arr[2].AsNumber(), 3.0);
}

TEST(JsonTest, JS03_ParseNestedStructures) {
    auto result = Json::Parse(R"({"arr":[{"k":true}]})");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsObject());

    const auto& arr = result->AsObject().at("arr").AsArray();
    ASSERT_EQ(arr.size(), 1u);
    EXPECT_TRUE(arr[0].IsObject());
    EXPECT_TRUE(arr[0].AsObject().at("k").AsBool());
}

TEST(JsonTest, JS04_RejectInvalidJson) {
    EXPECT_FALSE(Json::Parse("{a:1}").has_value());       // missing quotes
    EXPECT_FALSE(Json::Parse(R"({"a":1,})").has_value());  // trailing comma
    EXPECT_TRUE(Json::Parse(R"({"a":1})").has_value());    // valid JSON
    EXPECT_FALSE(Json::Parse(R"({"a":1)").has_value());    // unclosed brace
}

TEST(JsonTest, JS05_UnicodeEscape) {
    auto result = Json::Parse(R"({"c":"\u0041"})");
    ASSERT_TRUE(result.has_value());
    EXPECT_EQ(result->AsObject().at("c").AsString(), "A");
}

TEST(JsonTest, JS06_NumberPrecision) {
    auto result = Json::Parse(R"(9007199254740993)");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsNumber());
    // Large integers lose precision in double, but should not crash
    EXPECT_GT(result->AsNumber(), 0.0);
}

TEST(JsonTest, JS07_NullValue) {
    auto result = Json::Parse("null");
    ASSERT_TRUE(result.has_value());
    EXPECT_TRUE(result->IsNull());
}

TEST(JsonTest, JS08_BooleanValues) {
    auto t = Json::Parse("true");
    auto f = Json::Parse("false");
    ASSERT_TRUE(t.has_value());
    ASSERT_TRUE(f.has_value());
    EXPECT_TRUE(t->AsBool());
    EXPECT_FALSE(f->AsBool());
}

TEST(JsonTest, JS09_EmptyObjectAndArray) {
    auto obj = Json::Parse("{}");
    auto arr = Json::Parse("[]");
    ASSERT_TRUE(obj.has_value());
    ASSERT_TRUE(arr.has_value());
    EXPECT_TRUE(obj->IsObject());
    EXPECT_TRUE(arr->IsArray());
    EXPECT_EQ(obj->AsObject().size(), 0u);
    EXPECT_EQ(arr->AsArray().size(), 0u);
}

TEST(JsonTest, JS10_SerializeRoundtrip) {
    Json::Object obj;
    obj["name"] = "test";
    obj["value"] = 42.0;
    obj["flag"] = true;
    obj["nothing"] = nullptr;

    Json root(std::move(obj));
    std::string serialized = root.Dump();

    auto parsed = Json::Parse(serialized);
    ASSERT_TRUE(parsed.has_value());
    EXPECT_TRUE(parsed->IsObject());
    EXPECT_EQ(parsed->AsObject().at("name").AsString(), "test");
    EXPECT_EQ(parsed->AsObject().at("value").AsNumber(), 42.0);
    EXPECT_TRUE(parsed->AsObject().at("flag").AsBool());
    EXPECT_TRUE(parsed->AsObject().at("nothing").IsNull());
}

TEST(JsonTest, JS11_StringEscapeSequences) {
    auto result = Json::Parse(R"({"s":"line1\nline2\ttab"})");
    ASSERT_TRUE(result.has_value());
    const std::string& s = result->AsObject().at("s").AsString();
    EXPECT_EQ(s, "line1\nline2\ttab");
}

TEST(JsonTest, JS12_NegativeNumbers) {
    auto result = Json::Parse(R"({"neg":-42.5})");
    ASSERT_TRUE(result.has_value());
    EXPECT_DOUBLE_EQ(result->AsObject().at("neg").AsNumber(), -42.5);
}

} // namespace
} // namespace rex::runtime
