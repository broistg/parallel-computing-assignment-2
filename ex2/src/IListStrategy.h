#pragma once
#include <limits>
#include <string>

// Keys of the two sentinel nodes (head / tail). They replace the former `#define INT_MAX 50` / `#define INT_MIN 0`, which redefined the
// <climits> macros and made the key 50 collide with the tail sentinel (remove(50) unlinked and deleted the tail).
// User keys may be any int strictly between the two; the strategies return false for the sentinel keys themselves.
constexpr int kKeyMin = std::numeric_limits<int>::min();
constexpr int kKeyMax = std::numeric_limits<int>::max();

class IListStrategy {
public:
    virtual ~IListStrategy() = default;
    virtual bool add(int id, const std::string& name) = 0;
    virtual bool remove(int id) = 0;
    virtual bool contains(int id) = 0;
    virtual void print() = 0;
    // Only call validateStructure when no other thread is using the list (it reads the list without locks).
    virtual bool validateStructure(int expected_size = -1) = 0;
};
