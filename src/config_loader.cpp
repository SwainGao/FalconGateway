#include "config_loader.h"
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <nlohmann/json.hpp>

namespace falcon {

namespace {

using nlohmann::json;

std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

void readString(const json& j, const char* key, std::string& out) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_string()) throw std::runtime_error(std::string("\"") + key + "\" must be a string");
    out = it->get<std::string>();
}

void readStringList(const json& j, const char* key, std::vector<std::string>& out) {
    auto it = j.find(key);
    if (it == j.end()) return;
    if (!it->is_array()) throw std::runtime_error(std::string("\"") + key + "\" must be an array of strings");
    out.clear();
    for (const auto& v : *it) {
        if (!v.is_string()) throw std::runtime_error(std::string("\"") + key + "\" must be an array of strings");
        out.push_back(v.get<std::string>());
    }
}

void readEnv(const char* name, std::string& out) {
    if (const char* v = std::getenv(name); v && *v) out = v;
}

} // namespace

void parseConfig(const std::string& content, GatewayConfig& cfg) {
    json j;
    try {
        j = json::parse(content);
    } catch (const json::parse_error& e) {
        throw std::runtime_error(std::string("Invalid JSON: ") + e.what());
    }
    if (!j.is_object()) throw std::runtime_error("Config root must be a JSON object");

    readString(j, "broker_id", cfg.broker_id);
    readString(j, "user_id", cfg.user_id);
    readString(j, "password", cfg.password);
    readString(j, "app_id", cfg.app_id);
    readString(j, "auth_code", cfg.auth_code);
    readString(j, "trade_front", cfg.trade_front);
    readString(j, "market_front", cfg.market_front);
    readString(j, "flow_path_trade", cfg.flow_path_trade);
    readString(j, "flow_path_market", cfg.flow_path_market);
    readString(j, "product_filter", cfg.product_filter);
    readStringList(j, "subscribe_list", cfg.subscribe_list);
    // unknown keys are ignored (forward compatibility)

    // Secrets can be kept out of the file
    readEnv("FALCON_USER_ID", cfg.user_id);
    readEnv("FALCON_PASSWORD", cfg.password);
    readEnv("FALCON_AUTH_CODE", cfg.auth_code);
}

bool loadConfigFromFile(const std::string& path, GatewayConfig& cfg, std::string& err_msg) {
    try {
        parseConfig(readFile(path), cfg);
        return true;
    } catch (const std::exception& e) {
        err_msg = e.what();
        return false;
    }
}

GatewayConfig loadConfigFromFile(const std::string& path) {
    GatewayConfig cfg;
    std::string err;
    if (!loadConfigFromFile(path, cfg, err))
        throw std::runtime_error(err);
    return cfg;
}

} // namespace falcon
