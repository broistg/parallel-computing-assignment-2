# Concurrent Linked List (Strategy Design Pattern)

Dự án cài đặt cấu trúc dữ liệu **Danh sách liên kết đồng thời (Concurrent Linked List)** bằng ngôn ngữ C++, áp dụng **Strategy Design Pattern** để linh hoạt chuyển đổi giữa các chiến thuật đồng bộ hóa đa luồng tại thời điểm chạy (runtime).

## Điều kiện tiên quyết (Prerequisites)

Để biên dịch và chạy dự án, hệ thống cần đáp ứng các yêu cầu sau:

* **Trình biên dịch hỗ trợ chuẩn C++14 trở lên:** Bắt buộc để sử dụng `std::make_unique`, smart pointers (`std::unique_ptr`) và biểu thức lambda (ví dụ: `g++ >= 5.0`, `clang++ >= 3.4`, hoặc MSVC 2015+).
* **Thư viện đa luồng chuẩn C++ (`<thread>`, `<mutex>`):** Phục vụ việc khởi tạo luồng thực thi (`std::thread`) và cơ chế khóa tương tranh (`std::mutex`, `std::unique_lock`).
* **POSIX Threads (`pthread`):** Cần thiết trên các hệ điều hành nhân Linux/Unix/macOS khi liên kết trình biên dịch với cờ `-pthread`.

## Cấu trúc mã nguồn

.
├── ConcurrentList.h    # Lớp Context quản lý con trỏ chiến thuật (std::unique_ptr)[cite: 1]
├── IListStrategy.h     # Lớp Interface quản lý class chiến thuật (std::unique_ptr)[cite: 1]
├── strategies
    ├── FineGrained.h       # Cài đặt chiến thuật Fine-Grained Synchronization[cite: 1]
    ├── Optimistic.h        # Cài đặt chiến thuật Optimistic Synchronization[cite: 1]
└── main.cpp            # Chương trình chính: chọn chiến thuật và kiểm thử 4 luồng[cite: 1]

## Lệnh chạy mã nguồn
Trên Linux / macOS (GCC / Clang)
    g++ -std=c++14 -O2 -pthread main.cpp -o concurrent_list
    ./concurrent_list

Trên Windows
    g++ -std=c++14 -O2 main.cpp -o concurrent_list.exe
    concurrent_list.exe