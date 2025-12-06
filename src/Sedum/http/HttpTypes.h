//
// Created by user on 2025/12/5.
//

#ifndef HTTPTYPES_H
#define HTTPTYPES_H
#include <string>

namespace sedum {
    /// 表示通过 multipart/form-data 上传的文件
    /// 注意：data 字段是一个 string_view，指向 HttpRequest::body_ 的内容，避免了不必要的拷贝
    /// 使用时要确保 HttpRequest 对象的生命周期长于 MultipartFile 对象
    struct MultipartFile {
        std::string filename;    // 原始文件名 (e.g. "photo.jpg")
        std::string contentType; // 文件类型 (e.g. "image/jpeg")
        std::string_view data;   // 文件内容 (指向 HttpRequest::body_ 的视图，零拷贝)
        size_t size = 0;

        bool isValid() const { return !data.empty(); }
    };
}

#endif //HTTPTYPES_H
