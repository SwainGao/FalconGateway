#include "config_loader.h"
#include <fstream>
#include <sstream>
#include <stdexcept>
#include <cstring>

namespace falcon {

// Minimal flat-JSON parser — no external dependencies
// Handles a single-level JSON object with string values only
static std::string readFile(const std::string& path) {
    std::ifstream f(path);
    if (!f) throw std::runtime_error("Cannot open config file: " + path);
    std::ostringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

static std::string unescapeJson(const std::string& s) {
    // Minimal unescape: only handles the subset we need
    return s;  // config values don't contain escape sequences
}

static void skipWhitespace(const char*& p) {
    while (*p == ' ' || *p == '\t' || *p == '\n' || *p == '\r') ++p;
}

static std::string parseString(const char*& p) {
    if (*p != '"') throw std::runtime_error("Expected '\"'");
    ++p;
    const char* start = p;
    while (*p && *p != '"') {
        if (*p == '\\') ++p;  // skip escaped char
        if (*p) ++p;
    }
    std::string val(start, p - start);
    if (*p == '"') ++p;
    return val;
}

static void parseObject(const char*& p, GatewayConfig& cfg) {
    skipWhitespace(p);
    if (*p != '{') throw std::runtime_error("Expected '{'");
    ++p;

    while (*p) {
        skipWhitespace(p);
        if (*p == '}') { ++p; return; }
        if (*p == ',') { ++p; skipWhitespace(p); }

        std::string key = parseString(p);
        skipWhitespace(p);
        if (*p != ':') throw std::runtime_error("Expected ':' after key \"" + key + "\"");
        ++p;
        skipWhitespace(p);
        std::string val = parseString(p);

        // Map JSON keys to struct fields
        if (key == "broker_id")           cfg.broker_id = val;
        else if (key == "user_id")        cfg.user_id = val;
        else if (key == "password")       cfg.password = val;
        else if (key == "app_id")         cfg.app_id = val;
        else if (key == "auth_code")      cfg.auth_code = val;
        else if (key == "trade_front")    cfg.trade_front = val;
        else if (key == "market_front")   cfg.market_front = val;
        else if (key == "flow_path_trade")  cfg.flow_path_trade = val;
        else if (key == "flow_path_market") cfg.flow_path_market = val;
        // unknown keys are silently ignored (forward compatibility)
    }
}

bool loadConfigFromFile(const std::string& path, GatewayConfig& cfg, std::string& err_msg) {
    try {
        std::string content = readFile(path);
        const char* p = content.c_str();
        skipWhitespace(p);
        parseObject(p, cfg);
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
