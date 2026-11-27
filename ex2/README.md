# Section 2: Danh sách liên kết có thứ tự đồng thời (Strategy Design Pattern)

Dự án cài đặt cấu trúc dữ liệu **danh sách liên kết có thứ tự đồng thời (Concurrent Linked List)** bằng C++, áp dụng **Strategy Design Pattern** để chuyển đổi linh hoạt giữa các chiến thuật đồng bộ hóa đa luồng tại thời điểm chạy (runtime). Hai biến thể của đề bài được cài đặt: **khóa mịn (fine-grained, hand-over-hand, mỗi node một mutex)** và **đồng bộ lạc quan (optimistic: duyệt không khóa, khóa hai node, kiểm tra lại, thử lại nếu sai)**. Mỗi biến thể có `add`, `remove`, `contains`, `print` và `validateStructure` (kiểm tra cấu trúc khi không còn luồng nào chạy: lính canh còn nguyên, không có vòng, khóa tăng chặt, đúng số phần tử mong đợi).

## Điều kiện tiên quyết (Prerequisites)

* **Trình biên dịch hỗ trợ C++14 trở lên:** bắt buộc để dùng `std::make_unique`, smart pointer (`std::unique_ptr`) và biểu thức lambda (ví dụ `g++ >= 5.0`, `clang++ >= 3.4`, hoặc MSVC 2015+).
* **Thư viện đa luồng chuẩn C++ (`<thread>`, `<mutex>`):** để tạo luồng (`std::thread`) và dùng khóa (`std::mutex`, `std::unique_lock`).
* **POSIX Threads (`pthread`):** cần trên Linux/Unix/macOS khi liên kết với cờ `-pthread`.

## Cấu trúc mã nguồn

```
ex2/src/
  IListStrategy.h            interface của một chiến thuật; khóa lính canh kKeyMin / kKeyMax (khóa người dùng: mọi int nằm chặt giữa hai giá trị)
  ConcurrentList.h           lớp Context giữ con trỏ chiến thuật (std::unique_ptr)
  strategies/FineGrained.h   cài đặt chiến thuật Fine-Grained Synchronization
  strategies/Optimistic.h    cài đặt chiến thuật Optimistic Synchronization
  main.cpp                   chương trình kiểm thử: so sánh tuần tự với std::set, khóa lính canh, warm-up đồng thời, stress 8 luồng
```

## Lệnh biên dịch và chạy

Trên Linux / macOS (GCC / Clang), WSL2 cũng vậy:
```
g++ -std=c++14 -O2 -pthread ex2/src/main.cpp -o ex2_list
./ex2_list both            # hoặc: fine | optimistic   (không có tham số: hỏi lựa chọn qua stdin)
```
Trên Windows native: `g++ -std=c++14 -O2 ex2/src/main.cpp -o ex2_list.exe`, hoặc dùng target `ex2_list` của bản build CMake.
`make` và `./tests/run_tests.sh` của repo cũng biên dịch và chạy chương trình này (`make test-tsan` chạy dưới ThreadSanitizer).
Chương trình thoát với mã 0 và in `ALL EX2 CHECKS PASSED` khi mọi kiểm tra đều đạt.

## Ghi chú về cài đặt

* **Fine-grained:** khóa được lấy theo thứ tự trong list (pred rồi curr); node bị gỡ được xóa ngay sau khi nhả khóa, an toàn vì chỉ tới được một node qua node đứng trước nó đang bị khóa.
* **Optimistic:** lượt duyệt không khóa nên một luồng có thể đang đứng trên node mà luồng khác vừa gỡ. Con trỏ `next` là `std::atomic`, và node đã gỡ không bị xóa ngay mà đưa vào danh sách retired, giải phóng ở destructor (bản giáo trình dựa vào bộ gom rác). Cái giá: bộ nhớ của các node đã gỡ được giữ đến khi hủy list.
* Hai thuật toán này cũng có trong khung A* dưới dạng thích nghi với giao diện danh sách mở (`include/astar/fine_list.h`, `include/astar/optimistic_list.h`, loại lock là tham số template, có thao tác theo lô tùy chọn); báo cáo đo các bản đó.
