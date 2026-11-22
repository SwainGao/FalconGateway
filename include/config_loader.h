#pragma once
#include <string>
#include "types.h"

namespace falcon {

// 从 JSON 配置文件加载 GatewayConfig
// 键与 GatewayConfig 字段名一致；subscribe_list 为字符串数组，其余为字符串。
// 环境变量 FALCON_USER_ID / FALCON_PASSWORD / FALCON_AUTH_CODE 优先于文件内容。
GatewayConfig loadConfigFromFile(const std::string& path);

// 如果加载失败，返回 false 并将错误写入 err_msg
bool loadConfigFromFile(const std::string& path, GatewayConfig& cfg, std::string& err_msg);

// 解析 JSON 文本（失败时抛出 std::runtime_error）
void parseConfig(const std::string& content, GatewayConfig& cfg);

} // namespace falcon
