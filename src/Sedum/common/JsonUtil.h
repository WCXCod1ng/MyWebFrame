//
// Created by user on 2025/12/6.
//

#ifndef JSONUTIL_H
#define JSONUTIL_H
#include <string>
#include <nlohmann/json.hpp>
#include <nlohmann/json_fwd.hpp>

namespace common {
    using namespace nlohmann;
    class JsonUtil {
    public:
        template <typename T>
        static T fromJson(const std::string& jsonStr) {
            // 解析 JSON 字符串
            json j = json::parse(jsonStr);
            // 将 JSON 对象转换为指定类型的对象
            return j.get<T>();
        }
    };
}

#endif //JSONUTIL_H
