#include <gtest/gtest.h>
#include <cstdlib>
#include <stdexcept>
#include "config_loader.h"

using falcon::GatewayConfig;
using falcon::parseConfig;

class ConfigLoaderTest : public ::testing::Test {
protected:
    void SetUp() override {
        unsetenv("FALCON_USER_ID");
        unsetenv("FALCON_PASSWORD");
        unsetenv("FALCON_AUTH_CODE");
    }
};

TEST_F(ConfigLoaderTest, ParsesAllFields) {
    GatewayConfig cfg;
    parseConfig(R"({
        "broker_id": "9999", "user_id": "u", "password": "p\"w\\d",
        "app_id": "a", "auth_code": "c",
        "trade_front": "tcp://1.2.3.4:1", "market_front": "tcp://1.2.3.4:2",
        "flow_path_trade": "/t/", "flow_path_market": "/m/",
        "subscribe_list": ["rb2607", "TA701"], "product_filter": "rb",
        "unknown": {"nested": [1, 2]}
    })", cfg);
    EXPECT_EQ(cfg.broker_id, "9999");
    EXPECT_EQ(cfg.password, "p\"w\\d");
    EXPECT_EQ(cfg.flow_path_market, "/m/");
    ASSERT_EQ(cfg.subscribe_list.size(), 2u);
    EXPECT_EQ(cfg.subscribe_list[1], "TA701");
    EXPECT_EQ(cfg.product_filter, "rb");
}

TEST_F(ConfigLoaderTest, DecodesUnicodeEscapes) {
    GatewayConfig cfg;
    parseConfig(R"({"user_id": "\u4e2d"})", cfg);
    EXPECT_EQ(cfg.user_id, "\xE4\xB8\xAD");
}

TEST_F(ConfigLoaderTest, RejectsTrailingGarbage) {
    GatewayConfig cfg;
    EXPECT_THROW(parseConfig(R"({"user_id": "u"} junk)", cfg), std::runtime_error);
}

TEST_F(ConfigLoaderTest, RejectsWrongTypes) {
    GatewayConfig cfg;
    EXPECT_THROW(parseConfig(R"({"user_id": 123})", cfg), std::runtime_error);
    EXPECT_THROW(parseConfig(R"({"subscribe_list": "rb2607"})", cfg), std::runtime_error);
    EXPECT_THROW(parseConfig(R"({"subscribe_list": ["rb", 1]})", cfg), std::runtime_error);
    EXPECT_THROW(parseConfig(R"(["not", "object"])", cfg), std::runtime_error);
}

TEST_F(ConfigLoaderTest, EnvironmentOverridesSecrets) {
    setenv("FALCON_PASSWORD", "from-env", 1);
    GatewayConfig cfg;
    parseConfig(R"({"password": "from-file", "user_id": "u"})", cfg);
    EXPECT_EQ(cfg.password, "from-env");
    EXPECT_EQ(cfg.user_id, "u");
    unsetenv("FALCON_PASSWORD");
}

TEST_F(ConfigLoaderTest, MissingFileReportsError) {
    GatewayConfig cfg;
    std::string err;
    EXPECT_FALSE(falcon::loadConfigFromFile("/nonexistent/config.json", cfg, err));
    EXPECT_NE(err.find("Cannot open"), std::string::npos);
}
