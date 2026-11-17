# Bài tập lớn 2 (A* song song) — Báo cáo tiến độ: đã làm gì, đã quyết định gì và vì sao

Ngày: 17/11/2026 · Hạn nộp: 05/12/2026 · Repo: `parallel-computing-assignment-2`
Đối tượng đọc: ba thành viên nhóm (Đạt, Bình, Đăng). Bản tiếng Anh: `REPORT.md`.

## 1. Tóm tắt

Phần nền của Section 1 và Section 3 đã xong và đã được kiểm thử. Section 2 (các danh sách đồng thời khác) do các bạn khác làm và sẽ cắm vào một giao diện đã đóng băng. **Hiện CHƯA đo gì trên đồ thị HCMC thật hay trên máy của nhóm.** Môi trường phát triển không truy cập được OpenStreetMap, nên mọi thứ được xây dựng và kiểm thử trên một đồ thị giả lập giống mạng đường, đúng định dạng snapshot.

| Hạng mục | Trạng thái |
|----------|-----------|
| Công cụ đóng băng đồ thị (OSM → thành phần liên thông mạnh lớn nhất → file CSR số nguyên + hash) | đã viết; phần chuyển đổi đã test trên đồ thị dựng tay kiểu OSMnx; **chưa chạy export thật** |
| Chi phí phụ thuộc thời gian + hồ sơ tốc độ (đường cong giờ cao điểm giả lập) | xong, độ rộng bucket đổi được, đã test với 15 phút và 5 phút |
| Heuristic: chính xác, động/không chắc chắn (nhiễu, phóng đại) | xong |
| A* tuần tự (hàng đợi ưu tiên) | xong |
| A* song song, danh sách mở là tham số template | xong (mục tiêu đơn: thời gian hoặc quãng đường) |
| Danh sách mở: danh sách có thứ tự khóa thô với lock của A1 và `std::mutex` | xong; danh sách fine-grained/lazy của Section 2 cắm vào bằng một dòng |
| Oracle chính xác (Python): Dijkstra phụ thuộc thời gian, Martins cho Pareto | xong, đã đối chiếu với vét cạn |
| File golden gắn với dữ liệu đầu vào bằng dấu vân tay (fingerprint) | xong |
| A* vector (Pareto) bằng C++, tuần tự và song song | **mới chỉ có đặc tả** |
| 9 lỗi của notebook thành test hồi quy | **chưa bắt đầu** (chưa đọc lại notebook) |
| Script chạy thí nghiệm, script phân tích, quyết định ALT, nội dung báo cáo | chưa bắt đầu |

## 2. Những gì đã cài đặt

* `tools/export_graph.py`, `snapshot_io.py`: tạo snapshot (`nodes.csv`, `edges.csv`, `meta.json` có sha256). Đây là đoạn code duy nhất ghi snapshot. `synth_graph.py` tạo snapshot giả chỉ để test.
* `include/astar/graph.h`: nạp đồ thị vào CSR, hồ sơ tốc độ, chi phí cạnh phụ thuộc thời gian, kiểm tra FIFO, mã băm nội dung.
* `heuristic.h`: khoảng cách đường chim bay chia cho tốc độ tối đa; tùy chọn nhiễu và phóng đại.
* `openlist.h`: giao diện danh sách đã đóng băng, `CoarseList<Lock>`, lớp đo đạc (tỷ lệ các loại thao tác luôn có; độ trễ lấy mẫu khi yêu cầu).
* `search.h`: A* tuần tự, thread pool, A* song song, bộ kiểm tra đường đi.
* `src/bench_main.cpp`: chạy một cấu hình, ghi CSV kết quả 44 cột.
* `tools/oracle.py`, `make_queries.py`, `make_profile.py`, `inspect_platform.py`, `pilot.py`.
* `tests/`: `test_astar.cpp`, `test_oracle.py`, `check_gaps.py`, `run_tests.sh`.

## 3. Các quyết định và lý do

| # | Quyết định | Lý do | Phương án khác / đánh đổi | Đổi lại được không? |
|---|-----------|-------|---------------------------|---------------------|
| 1 | Đóng băng MỘT snapshot mạng đường HCMC, commit kèm hash; một người export, mọi người kiểm tra | OSM thay đổi hằng ngày; export lại sẽ âm thầm làm đổi mọi kết quả và file golden | mỗi người tự export (đồ thị khác nhau); dựa vào cache OSMnx (không mang đi được) | Không, sau khi đã có file golden |
| 2 | osmnx/NetworkX chỉ dùng để export; C++ đọc CSV thuần vào CSR | code được đo không phụ thuộc Python/thư viện; CSR thân thiện với cache | đọc GraphML trong C++ (nặng) | Được |
| 3 | Chỉ giữ thành phần liên thông mạnh lớn nhất | mọi truy vấn đều có đáp án; không có trường hợp "không tới được" làm nhiễu số đo | giữ tất cả rồi xử lý lỗi | Được, trước khi đóng băng |
| 4 | Đơn vị số nguyên: độ dài dm, thời gian ms, hệ số nhân phần nghìn; làm tròn lên | kết quả giống hệt trên mọi nền tảng, so sánh golden chính xác, không cần dung sai số thực | số thực double (bit cuối khác nhau giữa nền tảng) | Được |
| 5 | Độ dài cạnh ≥ khoảng cách đường chim bay + 1 dm (ép khi export, kiểm tra khi nạp) | giữ heuristic chim bay luôn chấp nhận được và nhất quán dù có làm tròn | tin độ dài OSM (có cạnh ngắn hơn đường chim bay) | Không |
| 6 | Chi phí phụ thuộc thời gian từ hồ sơ tốc độ: 4 loại đường × 96 bucket 15 phút, nội suy tuyến tính. **Đường cong là tự dựng** (không có dữ liệu giao thông thật); 15 phút là lựa chọn chủ quan, đổi sang 5 phút bằng cách sinh lại file | đề yêu cầu chi phí động; nội suy tránh bước nhảy | bucket mịn hơn (thêm độ phân giải cho dữ liệu tự dựng), hàm bậc thang (dễ vi phạm FIFO) | Được (điều khiển bằng dữ liệu, đã test với 96 và 288) |
| 7 | Kiểm tra FIFO (độ dốc < ½) khi nạp và trong test | Dijkstra/A* chỉ đúng khi xuất phát muộn hơn không bao giờ đến sớm hơn | cho phép không FIFO (cần thuật toán khác) | Không |
| 8 | Mục tiêu: thời gian di chuyển (chính), quãng đường (phụ), vector (thời gian, quãng đường) với quan hệ trội Pareto | khớp với f dạng vector của đề; thời gian là đại lượng phụ thuộc thời điểm | chỉ dùng tổng có trọng số (chỉ tìm được phần lồi của biên Pareto; giữ lại như xấp xỉ rẻ) | Được |
| 9 | Heuristic = khoảng cách đại cung / tốc độ tối đa (dòng chảy tự do). Chế độ "không chắc chắn": nhiễu xác định ±ρ theo (nút, bucket 15 phút, seed) và hệ số phóng đại w | chế độ chính xác là chấp nhận được và nhất quán (đảm bảo kết quả tối ưu); nhiễu/phóng đại mô phỏng ước lượng động và không chắc chắn một cách tái lập được (dùng hash, không dùng số ngẫu nhiên) | nhiễu ngẫu nhiên (không tái lập giữa các nền tảng) | Được |
| 10 | Mở lại (reopen) nút khi chi phí được cải thiện, không dùng tập đóng | tập đóng không an toàn khi mở rộng không theo thứ tự trong song song và khi heuristic không nhất quán | tập đóng + giả định nhất quán (hỏng ở chế độ nới lỏng) | Không |
| 11 | A* song song: chèn trùng + kiểm tra "cũ" (stale) thay vì decrease-key | danh sách có thứ tự dạng tập hợp không có decrease-key; phần tử cũ dễ phát hiện | xóa rồi chèn (hai thao tác, dễ race) | Được |
| 12 | Mỗi nút một từ 64-bit nguyên tử = (chi phí, nút cha), cập nhật bằng CAS-min | chi phí và nút cha không bao giờ lệch nhau; không dùng lock | khóa theo từng nút (tranh chấp nhiều hơn) | Được |
| 13 | Incumbent = chi phí đang lưu của đích; cắt tỉa phần tử có f ≥ incumbent | không cần biến chia sẻ thêm; an toàn khi h chấp nhận được | biến nguyên tử riêng cho cận | Được |
| 14 | Điều kiện dừng: bộ đếm `pending_items`, tăng TRƯỚC khi chèn, giảm SAU khi cha đã chèn xong mọi con | danh sách rỗng chưa đủ (một luồng có thể sắp chèn); đã kiểm tra bằng đột biến (mutation) | chia vòng bằng barrier (chậm hơn), cờ "mọi luồng rảnh" (dễ race) | Không |
| 15 | Danh sách mở là tham số template với API đóng băng (add, remove, contains, pop_min, size_approx) | các biến thể Section 2 cắm vào mà không sửa A*; so sánh công bằng | giao diện ảo virtual (thêm chi phí gọi cho mỗi thao tác) | API đã đóng băng |
| 16 | Dùng lại lock của A1; thêm `std::mutex` làm mốc thư viện | liên tục với A1, header đa nền tảng đã tin cậy, trả lời "lock nào hợp với danh sách"; `std::mutex` cho thấy spin có đáng không khi số luồng nhiều hơn số lõi. Chưa biên dịch với `lock.h` thật | chỉ dùng lock thư viện (mất so sánh với A1) | Được |
| 17 | Tách chân lý chính xác khỏi baseline có định hướng: oracle Python (độc lập) và A* C++ dùng hàng đợi ưu tiên | lỗi trong code dùng chung không thể tự che giấu; speedup đo so với code tuần tự tốt nhất, không phải oracle | so sánh các cài đặt với nhau | Không |
| 18 | Một file golden cho mọi nền tảng, kèm file phụ chứa dấu vân tay đầu vào, `bench` tự kiểm tra | chi phí là số nguyên nên kết quả không phụ thuộc nền tảng; dấu vân tay chặn việc dùng nhầm file cũ | golden riêng cho từng nền tảng (có thể che lỗi nền tảng) | Không |
| 19 | Bộ truy vấn được commit, không sinh lại; 5 khoảng cách, 50 cặp mỗi khoảng, 4 giờ xuất phát cố định (03:00, 08:00, 12:30, 17:30) luân phiên trong mỗi khoảng | bộ sinh ngẫu nhiên có seed của Python không đảm bảo giống nhau giữa các phiên bản; giờ xuất phát làm đổi độ khó nên mỗi khoảng có cùng tỷ lệ và có thể báo cáo theo từng giờ | giờ xuất phát ngẫu nhiên có seed (cũng tái lập được; dự kiến làm như một lần chạy kiểm tra độ nhạy) | Được |
| 20 | Quy trình: 10 lần lặp + 1 lần khởi động, lấy trung vị và IQR; mỗi cấu hình một tiến trình; thread pool tạo một lần; việc reset vùng nhớ không tính vào thời gian, ghi riêng là `init_ns`; chạy có/không ghim luồng ghi riêng (`pinned` là sự thật đo được); dòng smoke/pilot được gắn cờ và bỏ qua; ≥ 20 ms công việc cho mỗi (cấu hình, khoảng cách) được đo | loại nhiễu khởi tạo luồng và cấp phát; bộ nhớ đỉnh thuộc đúng cấu hình; macOS không ghim được | một tiến trình cho mọi cấu hình (bộ nhớ đỉnh bị chung) | Được |
| 21 | Tên cột của schema kết quả được đóng băng (tên biến C++ về sau đã đổi cho rõ hơn) | các script phân tích sau này phụ thuộc vào chúng | không | Đóng băng |
| 22 | Nền tảng: hai máy Windows (giả định WSL2 như A1) và một máy Apple M3; mỗi máy chạy `inspect_platform.py` trước tiên | phải báo cáo đúng phần cứng; WSL2 là máy ảo (vCPU có thể bị tạm dừng), M3 có lõi hiệu năng/tiết kiệm điện | bản build Windows native (có CMake, chưa test) | Được |
| 23 | Tối ưu: cắt tỉa theo incumbent, bố cục CSR, thành phần liên thông mạnh lớn nhất (đã xong); ALT landmark (quyết định ngày 22/11, tùy chọn theo đề); bỏ qua contraction hierarchies (giải thích trong báo cáo) | lợi ích trên công sức tốt nhất trong thời gian còn lại | CH (rất tốn công, CH phụ thuộc thời gian rất khó) | Còn mở |
| 24 | Chiến lược test: thang tin cậy nhiều tầng, kiểm tra đột biến chính các test, TSan; các test nặng giao cho máy của nhóm chạy | test không thể thất bại thì vô giá trị; môi trường phát triển chỉ có 2 lõi | chạy hết trong môi trường phát triển | Được |

## 4. Bằng chứng hiện có (đồ thị giả lập, máy 2 lõi)

* Oracle so với vét cạn mọi đường đi (thời gian, quãng đường, toàn bộ biên Pareto): 36 truy vấn, 0 sai lệch.
* C++ (tuần tự, song song 1 đến 16 luồng, 3 loại danh sách) so với Dijkstra độc lập và với file golden Python: khoảng 850.000 phép kiểm tra, 0 lỗi, với 96 và 288 bucket thời gian; file golden từ dữ liệu khác bị từ chối.
* Kiểm tra đột biến chính bộ test: giảm bộ đếm pending quá sớm → treo, bị watchdog bắt; dùng store thường thay cho CAS-min → bị test "búa tạ" riêng bắt (5/5 lần chạy khi máy rảnh, 0/3 khi có tiến trình nặng khác dùng chung hai lõi — khả năng phát hiện phụ thuộc thời điểm, nên TSan và lần chạy trên Apple M3 rất quan trọng); bỏ kiểm tra stale → vẫn đúng (chỉ là tối ưu).
* ThreadSanitizer (g++, x86-64, 2 lõi): 850.900 phép kiểm tra, 0 lỗi, không có báo cáo race, kể cả với đồ thị stress 48 nghìn nút (khoảng 7 phút). Lần chạy TSan trước đó mất hơn 10 phút là do lỗi trong test của tôi (các luồng chờ cờ bắt đầu bằng vòng lặp bận không nhường CPU nên bỏ đói nhau khi số luồng nhiều hơn số lõi); đã sửa, và thời gian từng giai đoạn mà test in ra giúp thấy ngay giai đoạn nào chậm.
* Pilot (đồ thị giả 48 nghìn nút, 2 lõi, lock thô): truy vấn tuần tự mất 15 µs đến 0,9 ms; A* song song mất khoảng 2,3 lần thời gian tuần tự với 1 luồng và khoảng 12 lần với 2 luồng. Đây là quan sát trên môi trường phát triển, không phải kết quả: nghi ngờ lock thô là nút thắt và máy chỉ có hai lõi. Nó cho thấy speedup song song không tự có, và truy vấn quá ngắn chủ yếu đo bộ đếm giờ, nên các khoảng quá ngắn cần bỏ hoặc gộp.

## 5. Rủi ro và việc còn mở

* Chưa có dữ liệu thật: export HCMC, truy vấn, file golden, pilot trên phần cứng thật (xem `TASKS.md`, phần A).
* Hồ sơ tốc độ là giả lập; báo cáo phải nói rõ.
* Code riêng cho Windows native và macOS (ghim luồng, đo bộ nhớ, script kiểm tra máy) chưa được test.
* Code mới chỉ biên dịch với bản thay thế của `lock.h`.
* Chưa có tìm kiếm Pareto và test hồi quy cho các lỗi của notebook.
* Đồ thị HCMC thật có thể có cạnh rất dài làm vi phạm kiểm tra FIFO (thông báo: "FIFO guard"); cách sửa nhỏ.

## 6. Cách tái lập

`./tests/run_tests.sh` (mọi tầng), `make test-tsan` (Linux, chậm), `python3 tools/inspect_platform.py --name <id>`, sau đó làm theo trình tự đóng băng trong `docs/TASKS.md` phần A.
