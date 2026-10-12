// Minimal dependency-free test framework
#pragma once

#include <cstdio>
#include <cstdint>
#include <functional>
#include <stdexcept>
#include <string>
#include <vector>

struct TestCase {
    const char* name;
    void (*fn)();
};

inline std::vector<TestCase>& Tests() {
    static std::vector<TestCase> t;
    return t;
}

struct Registrar {
    Registrar(const char* n, void (*f)()) { Tests().push_back({n, f}); }
};

#define DF_TEST(Name)                                                                       \
    static void Name();                                                                     \
    static Registrar reg_##Name(#Name, &Name);                                              \
    static void Name()

inline void Fail(const std::string& msg) { throw std::runtime_error(msg); }

#define CHECK(cond)                                                              \
    do {                                                                         \
        if (!(cond)) Fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                          "  CHECK(" #cond ") failed");                          \
    } while (0)

inline std::string Ts(const std::string& s) { return s; }
inline std::string Ts(const char* s) { return s ? s : ""; }
template <class T>
std::string Ts(const T& v) {
    return std::to_string(v);
}

#define CHECK_EQ(a, b)                                                                       \
    do {                                                                                     \
        auto va_ = (a);                                                                      \
        auto vb_ = (b);                                                                      \
        if (!(va_ == vb_))                                                                   \
            Fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) + "  CHECK_EQ(" #a \
                  ", " #b ") failed: " + Ts(va_) + " != " + Ts(vb_));                        \
    } while (0)

#define CHECK_THROWS(expr)                                                       \
    do {                                                                         \
        bool threw_ = false;                                                     \
        try {                                                                    \
            (void)(expr);                                                        \
        } catch (const std::exception&) {                                        \
            threw_ = true;                                                       \
        }                                                                        \
        if (!threw_) Fail(std::string(__FILE__) + ":" + std::to_string(__LINE__) + \
                          "  CHECK_THROWS(" #expr ") did not throw");            \
    } while (0)

inline int RunAllTests(const char* filter) {
    int passed = 0, failed = 0;
    for (auto& t : Tests()) {
        if (filter && *filter && std::string(t.name).find(filter) == std::string::npos) continue;
        std::printf("[ RUN  ] %s\n", t.name);
        try {
            t.fn();
            std::printf("[  OK  ] %s\n", t.name);
            ++passed;
        } catch (const std::exception& e) {
            std::printf("[ FAIL ] %s\n        %s\n", t.name, e.what());
            ++failed;
        }
    }
    std::printf("\n%d passed, %d failed\n", passed, failed);
    return failed == 0 ? 0 : 1;
}
