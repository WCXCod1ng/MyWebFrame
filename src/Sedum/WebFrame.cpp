//
// Created by user on 2025/11/30.
//

#include "WebFrame.h"

namespace sedum {
    // template<class... Ts> struct overloaded : Ts... { using Ts::operator()...; };
    // template<class... Ts> overloaded(Ts...) -> overloaded<Ts...>;
    // // --------------------------------
    //
    // int main() {
    //     std::variant<int, double, std::string> v = "Hello";
    //
    //     std::visit(overloaded{
    //         [](int arg) { std::cout << "It's int: " << arg << "\n"; },
    //         [](double arg) { std::cout << "It's double: " << arg << "\n"; },
    //         [](const std::string& arg) { std::cout << "It's string: " << arg << "\n"; }
    //     }, v);
    //
    //     return 0;
    // }
}