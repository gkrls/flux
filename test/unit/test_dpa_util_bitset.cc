#include <bitset>
#define DOCTEST_CONFIG_IMPLEMENT_WITH_MAIN // Provide main()
#include "doctest/doctest.h"

#include "dpa/util.h"

using Bitset64 = dpa::Bitset<64>;
using Bitset256 = dpa::Bitset<256>;

TEST_CASE("Bitset64") {
  Bitset64 bitset;

  SUBCASE("Initial state is empty") {
    CHECK(bitset.empty());
    CHECK(bitset.count() == 0);
  }

  SUBCASE("Set multiple bits") {
    // Set bits in each of the four words
    bitset.set(10);
    bitset.set(13);
    bitset.set(5);
    bitset.set(44);
    bitset.set(22);
    bitset.set(61);
    bitset.set(63);
    
    CHECK(bitset.check(10));
    CHECK(bitset.check(13));
    CHECK(bitset.check(5));
    CHECK(bitset.check(44));
    CHECK(bitset.check(22));
    CHECK(bitset.check(61));
    CHECK(bitset.check(63));
    CHECK_FALSE(bitset.empty());
    
    // Test clear() to clear all bits
    bitset.clear();
    CHECK_FALSE(bitset.check(10));
    CHECK_FALSE(bitset.check(13));
    CHECK_FALSE(bitset.check(5));
    CHECK_FALSE(bitset.check(44));
    CHECK_FALSE(bitset.check(22));
    CHECK_FALSE(bitset.check(61));
    CHECK_FALSE(bitset.check(63));
    CHECK(bitset.count() == 0);
    CHECK(bitset.empty());
  }

  SUBCASE("Set all bits") {
    for (int i = 0; i < 64; i++)
      bitset.set(i);
    
    CHECK(bitset.count() == 64);
    CHECK_FALSE(bitset.empty());
    
    // Check all bits are set
    for (int i = 0; i < 64; i++)
      CHECK(bitset.check(i));
  }

  SUBCASE("Boundary testing") {
    // Test at boundaries between words
    bitset.set(63);
    CHECK(bitset.check(63));

    // This is out of bounds and there is no OOB checking in bitset
    bitset.set(64);
    CHECK(!bitset.check(64));
    
    CHECK(bitset.count() == 1);
  }

  SUBCASE("Duplicate ops") {
    bitset.clear();
    bitset.set(1);
    bitset.set(1);
    CHECK(bitset.check(1));
    CHECK(bitset.count() == 1);
  }
}

TEST_CASE("Bitset256") {
  Bitset256 bitset;

  SUBCASE("Initial state is empty") {
    CHECK(bitset.empty());
    CHECK(bitset.count() == 0);
  }

  SUBCASE("Set and check individual bits") {
    // Test setting and checking bits across all four words
    for (int idx : {0, 63, 64, 127, 128, 191, 192, 255}) {
      bitset.set(idx);
      CHECK(bitset.check(idx));
      CHECK(bitset.count() == 1);
      CHECK_FALSE(bitset.empty());
      
      bitset.clear(idx);
      CHECK_FALSE(bitset.check(idx));
      CHECK(bitset.count() == 0);
      CHECK(bitset.empty());
    }
  }

  SUBCASE("Set multiple bits") {
    // Set bits in each of the four words
    bitset.set(10);
    bitset.set(70);
    bitset.set(130);
    bitset.set(200);
    
    CHECK(bitset.check(10));
    CHECK(bitset.check(70));
    CHECK(bitset.check(130));
    CHECK(bitset.check(200));
    CHECK(bitset.count() == 4);
    CHECK_FALSE(bitset.empty());
    
    // Test clear() to clear all bits
    bitset.clear();
    CHECK_FALSE(bitset.check(10));
    CHECK_FALSE(bitset.check(70));
    CHECK_FALSE(bitset.check(130));
    CHECK_FALSE(bitset.check(200));
    CHECK(bitset.count() == 0);
    CHECK(bitset.empty());
  }

  SUBCASE("Set all bits in a word") {
    // Set all bits in the first word (0-63)
    for (int i = 0; i < 64; i++) {
      bitset.set(i);
    }
    
    CHECK(bitset.count() == 64);
    CHECK_FALSE(bitset.empty());
    
    // Check all bits are set
    for (int i = 0; i < 64; i++) {
      CHECK(bitset.check(i));
    }
    
    // Check bits in other words are not set
    for (int i = 64; i < 256; i++) {
      CHECK_FALSE(bitset.check(i));
    }
  }

  SUBCASE("Set all bits in second word") {
    // Set all bits in the second word (64-127)
    for (int i = 64; i < 128; i++) {
      bitset.set(i);
    }
    
    CHECK(bitset.count() == 64);
    CHECK_FALSE(bitset.empty());
    
    // Check all bits in second word are set
    for (int i = 64; i < 128; i++) {
      CHECK(bitset.check(i));
    }
    
    // Check bits in other words are not set
    for (int i = 0; i < 64; i++) {
      CHECK_FALSE(bitset.check(i));
    }
    for (int i = 128; i < 256; i++) {
      CHECK_FALSE(bitset.check(i));
    }
  }

  SUBCASE("Set all 256 bits") {
    // Set all 256 bits
    for (int i = 0; i < 256; i++) {
      bitset.set(i);
    }
    
    CHECK(bitset.count() == 256);
    CHECK_FALSE(bitset.empty());
    
    // Check all bits are set
    for (int i = 0; i < 256; i++) {
      CHECK(bitset.check(i));
    }
    
    // Clear individual bits and verify count decreases
    bitset.clear(50);
    CHECK(bitset.count() == 255);
    
    bitset.clear(150);
    CHECK(bitset.count() == 254);
    
    // Clear all bits
    bitset.clear();
    CHECK(bitset.count() == 0);
    CHECK(bitset.empty());
  }

  SUBCASE("Boundary testing") {
    // Test at boundaries between words
    bitset.set(63);
    bitset.set(64);
    CHECK(bitset.check(63));
    CHECK(bitset.check(64));
    CHECK(bitset.count() == 2);
    
    bitset.set(127);
    bitset.set(128);
    CHECK(bitset.check(127));
    CHECK(bitset.check(128));
    CHECK(bitset.count() == 4);
    
    bitset.set(191);
    bitset.set(192);
    CHECK(bitset.check(191));
    CHECK(bitset.check(192));
    CHECK(bitset.count() == 6);
  }
} 