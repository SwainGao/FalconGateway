#pragma once
#include <string>
#include "types.h"

namespace falcon {

// 从 JSON 配置文件加载 GatewayConfig
// 配置文件格式：平坦的 JSON 对象，键与 GatewayConfig 字段名一致
GatewayConfig loadConfigFromFile(const std::string& path);

// 如果加载失败，返回 false 并将错误写入 err_msg
bool loadConfigFromFile(const std::string& path, GatewayConfig& cfg, std::string& err_msg);

} // namespace falcon
