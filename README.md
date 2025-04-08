# 🖱️ Giám sát stress thông qua hành vi sử dụng chuột – Driver USB + MQTT + MySQL

Dự án này xây dựng hệ thống giám sát hành vi sử dụng chuột máy tính (tốc độ, độ chính xác, số lần click...) để phục vụ nghiên cứu mối liên hệ giữa stress và thao tác chuột, dựa theo bài báo khoa học [PMC8052599](https://www.ncbi.nlm.nih.gov/pmc/articles/PMC8052599/).

## 🎯 Mục tiêu

- Viết **driver kernel** cho chuột USB có dây trên Linux.
- Thu thập và tính toán các chỉ số như: **vận tốc**, **độ chính xác**, **số lần click**.
- Gửi dữ liệu thông qua **MQTT** đến máy chủ.
- Lưu trữ dữ liệu vào **cơ sở dữ liệu MySQL** để phục vụ phân tích stress.
