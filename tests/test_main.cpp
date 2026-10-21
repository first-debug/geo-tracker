#include <cstring>
#include <exception>
#include <iostream>

#include "test.hpp"

// Запускает все тесты или только те, в имени которых есть подстрока argv[1].
// Код выхода 0 — все прошли, 1 — есть упавшие, 2 — ни один тест не подошёл.
int main(int argc, char **argv) {
    const char *filter = argc > 1 ? argv[1] : nullptr;
    int run = 0;
    int failed = 0;

    for (const test::Case &c : test::registry()) {
        if (filter != nullptr && std::strstr(c.name, filter) == nullptr) {
            continue;
        }
        run++;
        test::current_failed() = false;
        try {
            c.fn();
        } catch (const test::Abort &) {
            // REQUIRE уже отметил тест упавшим
        } catch (const std::exception &e) {
            test::fail(c.name, 0, std::string("unexpected exception: ") + e.what());
        } catch (...) {
            test::fail(c.name, 0, "unexpected exception");
        }

        if (test::current_failed()) {
            failed++;
            std::cout << "FAIL " << c.name << std::endl;
        } else {
            std::cout << "ok   " << c.name << std::endl;
        }
    }

    if (run == 0) {
        std::cout << "no tests match '" << (filter ? filter : "") << "'" << std::endl;
        return 2;
    }
    std::cout << std::endl << (run - failed) << "/" << run << " tests passed" << std::endl;
    return failed == 0 ? 0 : 1;
}
