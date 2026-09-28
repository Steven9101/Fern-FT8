// Fern-FT8, an FT8 decoder module for FernSDR.
// SPDX-License-Identifier: GPL-2.0-or-later
//
// A minimal test harness: TEST registers a function, CHECK records a failure
// and continues, REQUIRE records a failure and ends the test.
#pragma once

#include <ostream>
#include <sstream>
#include <string>
#include <type_traits>
#include <utility>
#include <vector>

namespace test {

struct Case {
    const char* name;
    void (*fn)();
};

std::vector<Case>& registry();

struct Register {
    Register(const char* name, void (*fn)()) { registry().push_back(Case{name, fn}); }
};

// Thrown by REQUIRE to leave the current test.
struct Stop {};

void report(const char* file, int line, const std::string& what);

// True when FERN_TEST_QUICK=1 asks for shorter statistical tests.
bool quick();

template <typename T, typename = void>
struct Streamable : std::false_type {};
template <typename T>
struct Streamable<T, std::void_t<decltype(std::declval<std::ostream&>() << std::declval<const T&>())>>
    : std::true_type {};

template <typename T>
std::string show(const T& v) {
    if constexpr (std::is_enum_v<T>) {
        return std::to_string(static_cast<long long>(v));
    } else if constexpr (std::is_same_v<T, std::string>) {
        return "\"" + v + "\"";
    } else if constexpr (Streamable<T>::value) {
        std::ostringstream os;
        os << v;
        return os.str();
    } else {
        return "<value>";
    }
}

}  // namespace test

#define TEST(name)                                                   \
    static void name();                                              \
    static const ::test::Register name##_registered(#name, name);    \
    static void name()

#define CHECK(cond)                                                  \
    do {                                                             \
        if (!(cond))                                                 \
            ::test::report(__FILE__, __LINE__, "CHECK(" #cond ")");  \
    } while (0)

#define REQUIRE(cond)                                                  \
    do {                                                               \
        if (!(cond)) {                                                 \
            ::test::report(__FILE__, __LINE__, "REQUIRE(" #cond ")");  \
            throw ::test::Stop{};                                      \
        }                                                              \
    } while (0)

#define CHECK_EQ(a, b)                                                                              \
    do {                                                                                            \
        const auto check_a_ = (a);                                                                  \
        const auto check_b_ = (b);                                                                  \
        if (!(check_a_ == check_b_))                                                                \
            ::test::report(__FILE__, __LINE__,                                                      \
                           "CHECK_EQ(" #a ", " #b "): " + ::test::show(check_a_) + " != " +         \
                               ::test::show(check_b_));                                             \
    } while (0)

// Checks that text contains part.
#define CHECK_HAS(text, part)                                                                       \
    do {                                                                                            \
        const std::string check_t_ = (text);                                                        \
        const std::string check_p_ = (part);                                                        \
        if (check_t_.find(check_p_) == std::string::npos)                                           \
            ::test::report(__FILE__, __LINE__, "CHECK_HAS: \"" + check_t_ + "\" lacks \"" + check_p_ + "\""); \
    } while (0)
