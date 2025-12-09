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
        /// 反序列化为对象
        /// @param jsonStr 待反序列化的json字符串
        template <typename T>
        static T fromJson(const std::string& jsonStr) {
            // 解析 JSON 字符串
            json j = json::parse(jsonStr);
            // 将 JSON 对象转换为指定类型的对象
            return j.get<T>();
        }

        /// 序列化为json
        /// @param obj 待序列化的对象，需要实现特定的方法
        template <typename T>
        static std::string toJson(T&& obj) {
            const json j = obj;
            auto jsonStr = j.dump();
            return jsonStr;
        }
    };
}

#endif //JSONUTIL_H
