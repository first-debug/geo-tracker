#ifndef TESTS_TEST_H
#define TESTS_TEST_H

/*
 * Минимальный каркас юнит-тестов без внешних зависимостей.
 *
 *   TEST(имя) { CHECK(условие); REQUIRE(условие); CHECK_NEAR(a, b, eps); ... }
 *
 * CHECK* при ошибке отмечают тест упавшим и продолжают его,
 * REQUIRE* — прерывают текущий тест. Запуск: build/tests/run-tests [подстрока имени].
 */

#include <cmath>
#include <functional>
#include <iostream>
#include <sstream>
#include <string>
#include <vector>

namespace test {

struct Case {
    const char *name;
    std::function<void()> fn;
};

inline std::vector<Case> &registry() {
    static std::vector<Case> cases;
    return cases;
}

struct Registrar {
    Registrar(const char *name, std::function<void()> fn) {
        registry().push_back({name, std::move(fn)});
    }
};

// исключение, которым REQUIRE прерывает тест
struct Abort {};

inline bool &current_failed() {
    static bool failed = false;
    return failed;
}

inline void fail(const char *file, int line, const std::string &what) {
    current_failed() = true;
    std::cerr << "  " << file << ":" << line << ": " << what << std::endl;
}

} // namespace test

#define TEST_CAT2(a, b) a##b
#define TEST_CAT(a, b) TEST_CAT2(a, b)

#define TEST(name)                                                              \
    static void TEST_CAT(test_fn_, __LINE__)();                                 \
    static test::Registrar TEST_CAT(test_reg_, __LINE__)(name, TEST_CAT(test_fn_, __LINE__)); \
    static void TEST_CAT(test_fn_, __LINE__)()

#define TEST_CHECK_IMPL(cond, text, on_fail)                                    \
    do {                                                                        \
        if (!(cond)) {                                                          \
            test::fail(__FILE__, __LINE__, text);                               \
            on_fail;                                                            \
        }                                                                       \
    } while (0)

#define CHECK(cond) TEST_CHECK_IMPL(cond, "CHECK(" #cond ")", (void)0)
#define REQUIRE(cond) TEST_CHECK_IMPL(cond, "REQUIRE(" #cond ")", throw test::Abort{})

#define TEST_CMP_IMPL(op, a, b, on_fail)                                        \
    do {                                                                        \
        const auto &test_a_ = (a);                                              \
        const auto &test_b_ = (b);                                              \
        if (!(test_a_ op test_b_)) {                                            \
            std::ostringstream test_os_;                                        \
            test_os_ << #a " " #op " " #b " (" << test_a_ << " vs " << test_b_ << ")"; \
            test::fail(__FILE__, __LINE__, test_os_.str());                     \
            on_fail;                                                            \
        }                                                                       \
    } while (0)

#define CHECK_EQ(a, b) TEST_CMP_IMPL(==, a, b, (void)0)
#define CHECK_LE(a, b) TEST_CMP_IMPL(<=, a, b, (void)0)
#define CHECK_LT(a, b) TEST_CMP_IMPL(<, a, b, (void)0)
#define REQUIRE_EQ(a, b) TEST_CMP_IMPL(==, a, b, throw test::Abort{})

#define CHECK_NEAR(a, b, eps)                                                   \
    do {                                                                        \
        const double test_a_ = (a);                                             \
        const double test_b_ = (b);                                             \
        if (!(std::fabs(test_a_ - test_b_) <= (eps))) {                         \
            std::ostringstream test_os_;                                        \
            test_os_.precision(12);                                             \
            test_os_ << "|" #a " - " #b "| <= " #eps " (" << test_a_ << " vs " << test_b_ << ")"; \
            test::fail(__FILE__, __LINE__, test_os_.str());                     \
        }                                                                       \
    } while (0)

// выражение должно бросить исключение типа type
#define CHECK_THROWS_AS(expr, type)                                             \
    do {                                                                        \
        bool test_thrown_ = false;                                              \
        try {                                                                   \
            (void)(expr);                                                       \
        } catch (const type &) {                                                \
            test_thrown_ = true;                                                \
        } catch (...) {                                                         \
        }                                                                       \
        if (!test_thrown_) {                                                    \
            test::fail(__FILE__, __LINE__, "CHECK_THROWS_AS(" #expr ", " #type ")"); \
        }                                                                       \
    } while (0)

// строка s содержит подстроку sub (для проверки текста ошибок)
#define CHECK_CONTAINS(s, sub)                                                  \
    do {                                                                        \
        const std::string test_s_ = (s);                                        \
        const std::string test_sub_ = (sub);                                    \
        if (test_s_.find(test_sub_) == std::string::npos) {                     \
            test::fail(__FILE__, __LINE__,                                      \
                       "\"" + test_s_ + "\" does not contain \"" + test_sub_ + "\""); \
        }                                                                       \
    } while (0)

#endif // TESTS_TEST_H
