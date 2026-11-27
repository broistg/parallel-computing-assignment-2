#pragma once
#include <limits>
#include <string>

// Khóa của hai node lính canh (head / tail). Thay cho `#define INT_MAX 50` / `#define INT_MIN 0` trước đây: hai dòng đó định nghĩa lại
// macro của <climits> và làm khóa 50 trùng với lính canh đuôi (remove(50) gỡ và xóa luôn node đuôi).
// Khóa của người dùng là mọi số int nằm chặt giữa hai giá trị này; các chiến thuật trả về false với chính các khóa lính canh.
constexpr int kKeyMin = std::numeric_limits<int>::min();
constexpr int kKeyMax = std::numeric_limits<int>::max();

class IListStrategy {
public:
    virtual ~IListStrategy() = default;
    virtual bool add(int id, const std::string& name) = 0;
    virtual bool remove(int id) = 0;
    virtual bool contains(int id) = 0;
    virtual void print() = 0;
    // Chỉ gọi validateStructure khi không còn luồng nào đang dùng list (hàm đọc list mà không khóa).
    virtual bool validateStructure(int expected_size = -1) = 0;
};
