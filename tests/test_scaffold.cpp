#include <catch2/catch_test_macros.hpp>

#include <string_view>
#include <utility>

TEST_CASE("Scaffold sanity check", "[scaffold]") {
    REQUIRE(1 + 1 == 2);

    // Test std::string_view (C++17)
    std::string_view sv = "hello";
    REQUIRE(sv.length() == 5);

    // Test structured bindings (C++17)
    auto pair = std::make_pair(1, 2);
    auto [a, b] = pair;
    REQUIRE(a == 1);
    REQUIRE(b == 2);
}
