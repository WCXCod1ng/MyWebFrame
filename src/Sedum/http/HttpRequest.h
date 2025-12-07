//
// Created by user on 2025/11/29.
//

#ifndef HTTPREQUEST_H
#define HTTPREQUEST_H
#include <string>
#include <unordered_map>
#include <base/utils.h>

#include "base/NonCopyable.h"
#include "base/TimeStamp.h"

namespace fleabane {
	class Buffer;
    class NonCopyable;
    class TimeStamp;
}

namespace sedum {
    using namespace fleabane;

    /// 定义该HTTPConnection所支持的请求方法，暂时只支持GET和POST两种
    enum class Method {
        kInvalid, kGet, kPost, kHead, kPut, kDelete, kOptions
    };

    /// HTTP 版本，目前仅考虑HTTP1.x
    enum class Version {
        kUnknown, kHttp10, kHttp11
    };

    class HttpRequest : NonCopyable {
    public:
        HttpRequest() : method_(Method::kInvalid), version_(Version::kUnknown) {}


        /// 注意，要想让一个对象可以被安全移动，要满足两个基本条件
        /// 1. 移动操作是安全的，能够顺利窃取原始对象的资源（或者对于trivial类型直接拷贝）
        /// 2. 移动后原始对象是可析构的（不会影响目标对象），这里因为都是用容器管理，它们在移动后天生满足“可析构”

        /// 支持移动构造
        HttpRequest(HttpRequest&& rhs) noexcept {
            swap(rhs); // 直接调用swap函数
        }
        /// 支持移动赋值
        HttpRequest& operator=(HttpRequest&& rhs) noexcept {
            swap(rhs);
            return *this;
        }

        // --- Getters / Setters ---

        void setVersion(const Version v) { version_ = v; }
        Version getVersion() const { return version_; }

        // 设置方法：从 buffer 中解析出的字符串转换
        bool setMethod(const char* start, const char* end) {
            std::string m(start, end);
            if (m == "GET") method_ = Method::kGet;
            else if (m == "POST") method_ = Method::kPost;
            else if (m == "HEAD") method_ = Method::kHead;
            else if (m == "PUT") method_ = Method::kPut;
            else if (m == "DELETE") method_ = Method::kDelete;
            else method_ = Method::kInvalid;

            return method_ != Method::kInvalid;
        }
        Method method() const { return method_; }

        /// 辅助：获取方法的字符串表示
        const char* methodString() const {
            switch(method_) {
                case Method::kGet: return "GET";
                case Method::kPost: return "POST";
                case Method::kHead: return "HEAD";
                case Method::kPut: return "PUT";
                case Method::kDelete: return "DELETE";
                default: return "UNKNOWN";
            }
        }

        /// 设置路径 /index.html?id=1
        void setUrl(const char* start, const char* end) {
            url_.assign(start, end);
        }
        /// 获取url
        const std::string& url() const { return url_; }

        /// 设置查询参数
        void setQueries(const char * start, const char * end) {
            // 调用内部的解析逻辑进行处理，并把结果存储到queries中
            parse_queries(std::string_view(start, end), queries_);
        }

        /// 获取所有查询参数
        const std::unordered_map<std::string, std::string>& getQueries() const {
            return queries_;
        }

        /// 解析表单数据
        void parseFormData() {
            if(method_ != Method::kPost) {
                return; // 仅处理POST请求
            }
            auto ct = getHeader("Content-Type");
            if(!ct.empty() && ct == "application/x-www-form-urlencoded") {
                // 解析body中的表单数据，直接复用之前的解析函数，并将结果存储到form_data_中
                parse_queries(std::string_view(body_.data(), body_.size()), form_data_);
            }
        }

        /// 获取表单数据
        const std::unordered_map<std::string, std::string>& getFormData() {
            if(form_data_.empty()) {
                parseFormData();
            }
            return form_data_;
        }

        /// 设置接收时间
        void setReceiveTime(TimeStamp t) { receiveTime_ = t; }
        TimeStamp receiveTime() const { return receiveTime_; }

        /// 添加头部字段（一次添加一个）
        void addHeader(const char* start, const char* colon, const char* end) {
            // key: [start, colon)
            std::string field(start, colon);
            ++colon; // 跳过冒号
            // 去除 value 前面的空格
            while (colon < end && isspace(*colon)) {
                ++colon;
            }
            // value: [colon, end)
            std::string value(colon, end);

            // 去除 value 后面的空格
            while (!value.empty() && isspace(value[value.size()-1])) {
                value.resize(value.size()-1);
            }

            headers_[field] = value;
        }

        /// 获取头部字段，若不存在返回空串
        std::string getHeader(const std::string& field) const {
            std::string result;
            if (const auto it = headers_.find(field); it != headers_.end()) {
                result = it->second;
            }
            return result;
        }

        const std::unordered_map<std::string, std::string>& headers() const { return headers_; }

        /// 设置请求体
        void setBody(const char* start, const char* end) {
            body_.assign(start, end);
        }

        /// 获取请求体
        const std::string& getBody() const {
            return body_;
        }

        // 交换数据（用于高性能移动）
        void swap(HttpRequest& that) noexcept {
            std::swap(method_, that.method_);
            std::swap(version_, that.version_);
            url_.swap(that.url_);
            queries_.swap(that.queries_);
            receiveTime_.swap(that.receiveTime_);
            headers_.swap(that.headers_);
            body_.swap(that.body_); // 如果有 body 的话
        }

    private:

        /// 内部函数，专用于解析查询参数
        /// @param query_string 格式类似于 key1=value1&key2=value2&key3=value3
        /// @param res 查询参数存储的结果
        static bool parse_queries(std::string_view query_string, std::unordered_map<std::string, std::string>& res) {
            std::size_t start = 0;
            while(start < query_string.size()) {
                std::size_t ampersand_pos = query_string.find_first_of('&', start);
                if(ampersand_pos == std::string_view::npos) {
                    ampersand_pos = query_string.size(); // 默认放到结尾，方便start统一处理
                }
                // 这样[start, ampersand_pos]之间就是一个完整的key=value对
                std::string_view entry = query_string.substr(start, ampersand_pos - start);

                // 更新start的位置为“&”的下一个位置，ampersand_pos已经特判最后一个字符了
                start = ampersand_pos + 1;

                // 如果entry为空，跳过
                if(entry.empty()) {
                    continue;
                }

                // 解析key和value
                const std::size_t equal_pos = entry.find_first_of('=');
                std::string_view key_sv;
                std::string_view value_sv;
                if(equal_pos == std::string_view::npos) {
                    // note 找不到“=”，则直接认为是只有key，默认value为空字符串
                    key_sv = entry.substr(0);
                    value_sv = "";
                } else {
                    // 第一个“=”之前的被认为是key，第一个“=”之后的被认为是value。这是行业的一般遵循标准
                    key_sv = entry.substr(0, equal_pos);
                    value_sv = entry.substr(equal_pos + 1);
                }

                // note 生产环境需要对key和value进行解码（因为可能会出现非ASCII字符或“/”、“&”、“.”等特殊字符）

                auto key = url_decode(key_sv);
                auto value = url_decode(value_sv);

                // 加入到结果中
                res[std::move(key)] = std::move(value);
            }

            return true; // 除非未来有严格的格式要求，否则总是返回 true
        }


        Method method_; // 请求方式
        Version version_; // 协议版本
        std::string url_;  // 资源路径
        std::unordered_map<std::string, std::string> queries_; // 查询参数
        std::unordered_map<std::string, std::string> form_data_; // 表单数据
        TimeStamp receiveTime_; // 请求到达时间
        std::unordered_map<std::string, std::string> headers_; // 请求头
        std::string body_; // 请求体
    };
}



#endif //HTTPREQUEST_H
