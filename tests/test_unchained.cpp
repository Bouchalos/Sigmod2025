#include <catch2/catch_test_macros.hpp>
#include "../src/UnchainedHash.h"
#include <vector>
#include <string>

TEST_CASE("UnchainedHashTable basic insert/find operations", "[insert][find]") {
    UnchainedHashTable<int, int> table(8);

    table.add_tuple(1, 100);
    table.add_tuple(2, 200);
    table.add_tuple(3, 300);

    table.build();

    auto v1 = table.find(1);
    auto v2 = table.find(2);
    auto v3 = table.find(3);
    auto v4 = table.find(4); 

    REQUIRE(v1.size() == 1);
    REQUIRE(*v1[0] == 100);

    REQUIRE(v2.size() == 1);
    REQUIRE(*v2[0] == 200);

    REQUIRE(v3.size() == 1);
    REQUIRE(*v3[0] == 300);

    REQUIRE(v4.empty());
}

TEST_CASE("UnchainedHashTable handles duplicate keys", "[insert][duplicate]") {
    UnchainedHashTable<int, std::vector<size_t>> table(8);

    table.add_tuple(1, {10});
    table.add_tuple(1, {20});  
    table.add_tuple(2, {30});

    table.build();

    auto vec1 = table.find(1);
    auto vec2 = table.find(2);
    auto vec3 = table.find(3);

    REQUIRE(vec1.size() == 2); 
    REQUIRE(vec2.size() == 1);
    REQUIRE(vec3.empty());
}

TEST_CASE("UnchainedHashTable bloom filter rejects missing keys", "[bloom]") {
    UnchainedHashTable<int, int> table(4);

    table.add_tuple(4, 999);
    table.build();

    auto missing = table.find(99);
    REQUIRE(missing.empty());

    auto present = table.find(4);
    REQUIRE(present.size() == 1);
    REQUIRE(*present[0] == 999);
}

TEST_CASE("UnchainedHashTable empty table behavior", "[empty]") {
    UnchainedHashTable<int, int> table(0);
    table.build();

    REQUIRE(table.find(0).empty());
    REQUIRE(table.find(12345).empty());
}

TEST_CASE("UnchainedHashTable large number of elements", "[stress][large]") {
    UnchainedHashTable<int, int> table(1000);

    for (int i = 0; i < 1000; ++i) {
        table.add_tuple(i, i * 10);
    }
    table.build();

    for (int i = 0; i < 1000; ++i) {
        auto val = table.find(i);
        REQUIRE(val.size() == 1);
        REQUIRE(*val[0] == i * 10);
    }

    auto missing = table.find(1001);
    REQUIRE(missing.empty());
}
