// A test harness small enough to read in one sitting, so the project has no
// external dependency just to run assertions.
#pragma once

#include <iostream>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>

namespace test {

inline int g_checks = 0;
inline int g_failures = 0;

inline void report(bool passed, std::string_view expression, std::string_view file,
                   int line, const std::string& detail) {
    ++g_checks;
    if (passed) {
        return;
    }
    ++g_failures;
    std::cout << "   FAIL " << file << ':' << line << ": " << expression;
    if (!detail.empty()) {
        std::cout << "  (" << detail << ')';
    }
    std::cout << '\n';
}

inline void section(std::string_view name) { std::cout << "-- " << name << '\n'; }

// A fold expression cannot live inside a macro's __VA_ARGS__, so the variadic
// part goes in a helper the macro forwards to.
template <typename... Args>
std::string join(Args&&... args) {
    std::ostringstream oss;
    (oss << ... << std::forward<Args>(args));
    return oss.str();
}

inline int summary() {
    std::cout << g_checks << " checks, " << g_failures << " failures\n";
    return g_failures == 0 ? 0 : 1;
}

}  // namespace test

#define CHECK(expr) \
    ::test::report((expr), #expr, __FILE__, __LINE__, "")

#define CHECK_MSG(expr, ...) \
    ::test::report((expr), #expr, __FILE__, __LINE__, ::test::join(__VA_ARGS__))

// Values, not references: binding `const auto&` to a member of a temporary
// (e.g. `table.lookup(x)->out_port`) leaves a dangling reference, which GCC
// correctly rejects under -Wdangling-pointer.
#define CHECK_EQ(lhs, rhs)                                                   \
    do {                                                                     \
        const auto _l = (lhs);                                               \
        const auto _r = (rhs);                                               \
        std::ostringstream _oss;                                             \
        _oss << "got " << _l << ", expected " << _r;                         \
        ::test::report(_l == _r, #lhs " == " #rhs, __FILE__, __LINE__, _oss.str()); \
    } while (0)
