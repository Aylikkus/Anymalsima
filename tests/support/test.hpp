#pragma once

#include <functional>
#include <iostream>
#include <map>
#include <sstream>
#include <stdexcept>
#include <string>

namespace test {
using Case = std::function<void()>;
inline auto& cases() {
    static std::map<std::string, Case> result;
    return result;
}
struct Register {
    Register(const std::string& name, Case fn) { cases().emplace(name, fn); }
};
template <typename Actual, typename Expected>
void equal(const Actual& actual, const Expected& expected,
           const std::string& context, const char* file, int line) {
    if (actual == expected) return;
    std::ostringstream message;
    message << file << ':' << line << ": " << context << ": expected "
            << expected << ", actual " << actual;
    throw std::runtime_error(message.str());
}
}  // namespace test

#define TEST(name)                                  \
    static void name();                             \
    static test::Register reg_##name(#name, &name); \
    static void name()
#define EQUAL(actual, expected, context) \
    test::equal((actual), (expected), (context), __FILE__, __LINE__)
#define CHECK(condition) EQUAL(bool(condition), true, #condition)
