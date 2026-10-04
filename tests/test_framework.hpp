#pragma once

#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace bmrf_test {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& registry()
{
    static std::vector<TestCase> tests;
    return tests;
}

inline int& failureCount()
{
    static int failures = 0;
    return failures;
}

struct Registrar {
    Registrar(const std::string& name, std::function<void()> fn)
    {
        registry().push_back({name, std::move(fn)});
    }
};

inline int runAll()
{
    int failedTests = 0;
    for (const auto& test : registry()) {
        const int before = failureCount();
        std::cout << "[ RUN  ] " << test.name << std::endl;
        test.fn();
        if (failureCount() > before) {
            ++failedTests;
            std::cout << "[ FAIL ] " << test.name << std::endl;
        } else {
            std::cout << "[  OK  ] " << test.name << std::endl;
        }
    }
    const size_t total = registry().size();
    const size_t passed = total - static_cast<size_t>(failedTests);
    std::cout << passed << "/" << total << " tests passed" << std::endl;
    return failedTests == 0 ? 0 : 1;
}

} // namespace bmrf_test

#define BMRF_TEST(name)                                                   \
    static void name();                                                   \
    static ::bmrf_test::Registrar bmrf_registrar_##name(#name, name);     \
    static void name()

#define BMRF_CHECK(condition)                                             \
    do {                                                                  \
        if (!(condition)) {                                               \
            std::cerr << "  FAIL: " << #condition << " at " << __FILE__   \
                      << ":" << __LINE__ << std::endl;                    \
            ++::bmrf_test::failureCount();                                \
        }                                                                 \
    } while (false)

#define BMRF_CHECK_EQ(a, b)                                               \
    do {                                                                  \
        if (!((a) == (b))) {                                              \
            std::cerr << "  FAIL: " << #a << " == " << #b << " at "       \
                      << __FILE__ << ":" << __LINE__ << std::endl;        \
            ++::bmrf_test::failureCount();                                \
        }                                                                 \
    } while (false)
