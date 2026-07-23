#include "test_settings_fixtures.hpp"

using namespace rex::settings;

TEST(SettingsTypeTest, ToString) {
    EXPECT_STREQ(toString(SettingType::Boolean), "Boolean");
    EXPECT_STREQ(toString(SettingType::Integer), "Integer");
    EXPECT_STREQ(toString(SettingType::String), "String");
}

TEST(SettingsValueTest, TypeOf) {
    EXPECT_EQ(typeOf(true), SettingType::Boolean);
    EXPECT_EQ(typeOf(int64_t(42)), SettingType::Integer);
    EXPECT_EQ(typeOf(3.14), SettingType::Float);
    EXPECT_EQ(typeOf(std::string("hello")), SettingType::String);
    EXPECT_EQ(typeOf(std::vector<std::string>{}), SettingType::List);
    EXPECT_EQ(typeOf(std::vector<uint8_t>{}), SettingType::Object);
}

TEST_F(SettingsServiceTest, RegisterAndGetValue) {
    Setting s;
    s.id = "desktop.theme.dark_mode";
    s.type = SettingType::Boolean;
    s.defaultValue = false;
    ASSERT_TRUE(service->registerSetting(s));

    auto val = service->getValue("desktop.theme.dark_mode");
    ASSERT_TRUE(val.has_value());
    EXPECT_FALSE(std::get<bool>(*val));
}

TEST_F(SettingsServiceTest, SetValueWithValidation) {
    Setting s;
    s.id = "performance.cpu_limit";
    s.type = SettingType::Integer;
    s.defaultValue = int64_t(100);
    s.validators.push_back(validators::range(int64_t(0), int64_t(100)));
    ASSERT_TRUE(service->registerSetting(s));

    EXPECT_TRUE(service->setValue("performance.cpu_limit", int64_t(50)));
    EXPECT_EQ(std::get<int64_t>(*service->getValue("performance.cpu_limit")), 50);
    EXPECT_FALSE(service->setValue("performance.cpu_limit", int64_t(150)));
    EXPECT_EQ(std::get<int64_t>(*service->getValue("performance.cpu_limit")), 50);
}

TEST_F(SettingsServiceTest, TransactionAtomicity) {
    Setting a;
    a.id = "a";
    a.type = SettingType::String;
    a.defaultValue = std::string("initial");
    a.validators.push_back(validators::regex("^[a-z]+$"));
    service->registerSetting(a);

    Setting b;
    b.id = "b";
    b.type = SettingType::String;
    b.defaultValue = std::string("initial");
    b.validators.push_back(validators::regex("^[a-z]+$"));
    service->registerSetting(b);

    auto txn = service->beginTransaction();
    service->setValue("a", std::string("valid"));
    service->setValue("b", std::string("123"));
    service->commitTransaction(std::move(txn));

    EXPECT_NE(std::get<std::string>(*service->getValue("a")), "valid");
    EXPECT_EQ(std::get<std::string>(*service->getValue("b")), "initial");
}

TEST_F(SettingsServiceTest, ResetToDefault) {
    Setting s;
    s.id = "test.greeting";
    s.type = SettingType::String;
    s.defaultValue = std::string("hello");
    ASSERT_TRUE(service->registerSetting(s));

    service->setValue("test.greeting", std::string("world"));
    EXPECT_EQ(std::get<std::string>(*service->getValue("test.greeting")), "world");

    service->resetSetting("test.greeting");
    EXPECT_EQ(std::get<std::string>(*service->getValue("test.greeting")), "hello");
}

TEST(SettingsRegistryTest, AddAndGetSetting) {
    SettingsRegistry reg;

    Setting s;
    s.id = "test.bool";
    s.name = "Test Bool";
    s.type = SettingType::Boolean;
    s.defaultValue = false;

    EXPECT_TRUE(reg.addSetting(s));
    EXPECT_FALSE(reg.addSetting(s));

    auto val = reg.getValue("test.bool");
    ASSERT_TRUE(val.has_value());
    EXPECT_TRUE(std::holds_alternative<bool>(*val));
    EXPECT_FALSE(std::get<bool>(*val));
}

TEST(ValidatorsTest, RangeValidator) {
    auto v = validators::range(int64_t(0), int64_t(100));

    auto r1 = v(int64_t(50));
    EXPECT_TRUE(r1.valid);

    auto r2 = v(int64_t(200));
    EXPECT_FALSE(r2.valid);
}

TEST(ValidatorsTest, OneOfValidator) {
    auto v = validators::oneOf({"red", "green", "blue"});

    EXPECT_TRUE(v(std::string("red")).valid);
    EXPECT_TRUE(v(std::string("blue")).valid);
    EXPECT_FALSE(v(std::string("yellow")).valid);
}

TEST(ValidatorsTest, RegexValidator) {
    auto v = validators::regex("^[a-z]+$");

    EXPECT_TRUE(v(std::string("hello")).valid);
    EXPECT_FALSE(v(std::string("Hello123")).valid);
}

TEST(SettingsTransactionTest, RollbackOnFailure) {
    SettingsTransaction txn;
    int applied = 0;
    txn.addChange([&]() { applied++; return true; },
                  []() {});
    txn.addChange([&]() { applied++; return false; },
                  []() {});
    EXPECT_FALSE(txn.commit());
    EXPECT_EQ(applied, 2);
}
