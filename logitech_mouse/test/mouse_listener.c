#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <string.h>
#include <math.h>
#include <errno.h>

struct mouse_event {
    long long timestamp_sec;
    long timestamp_nsec;
    int type;
    int x;
    int y;
    int button;
    int action;
    int wheel_value;
};

struct trajectory_point {
    double timestamp;
    int delta_x;
    int delta_y;
    int type; // Thêm type để phân biệt MOVE, CLICK, WHEEL
};

#define MAX_POINTS 10000
#define DEVICE_PATH "/dev/logitech_mouse"
#define ANGLE_TOLERANCE 0.1 // Ngưỡng sai số cho góc (radian)

void process_trajectory(struct trajectory_point *points, int count) {
    if (count < 2) return;

    // Tổng thời gian bao gồm cả điểm cuối (CLICK hoặc WHEEL)
    double total_time = points[count - 1].timestamp - points[0].timestamp;
    if (total_time < 1.0 || total_time > 10.0) {
        printf("Trajectory không hợp lệ: thời gian = %.3f giây (phải từ 1 đến 10 giây)\n", total_time);
        return;
    }

    // Tính tổng khoảng cách (chỉ tính các đoạn MOVE)
    double total_distance = 0.0;
    for (int i = 0; i < count - 1; i++) {
        if (points[i].type == 0 && points[i + 1].type == 0) { // Chỉ tính giữa các MOVE
            double dx = (double)points[i + 1].delta_x;
            double dy = (double)points[i + 1].delta_y;
            total_distance += sqrt(dx * dx + dy * dy);
        }
    }

    double speed = total_distance / total_time;

    // Tính accuracy dựa trên góc giữa các đoạn MOVE liên tiếp
    int eqdir_count = 0;
    int valid_segments = 0;
    for (int i = 0; i < count - 2; i++) {
        if (points[i].type == 0 && points[i + 1].type == 0 && points[i + 2].type == 0) {
            double dx1 = (double)points[i + 1].delta_x;
            double dy1 = (double)points[i + 1].delta_y;
            double dx2 = (double)points[i + 2].delta_x;
            double dy2 = (double)points[i + 2].delta_y;

            // Tính góc bằng arctan2 và kiểm tra sai số
            double angle1 = atan2(dy1, dx1);
            double angle2 = atan2(dy2, dx2);
            double angle_diff = fabs(angle1 - angle2);
            if (angle_diff > M_PI) angle_diff = 2 * M_PI - angle_diff; // Chuẩn hóa góc
            if (angle_diff < ANGLE_TOLERANCE) {
                eqdir_count++;
            }
            valid_segments++;
        }
    }
    double accuracy = (valid_segments > 0) ? (double)eqdir_count / valid_segments : 1.0;

    printf("Trajectory hợp lệ: %d điểm, thời gian = %.3f giây\n", count, total_time);
    printf("  Speed = %.3f (đơn vị tương đối/giây)\n", speed);
    printf("  Accuracy = %.3f\n", accuracy);
}

int main() {
    int fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        perror("Không thể mở thiết bị");
        return 1;
    }

    struct mouse_event event;
    struct trajectory_point points[MAX_POINTS];
    int point_count = 0;

    printf("Bắt đầu theo dõi sự kiện chuột...\n");

    while (1) {
        ssize_t bytes_read = read(fd, &event, sizeof(struct mouse_event));
        if (bytes_read < 0) {
            perror("Lỗi khi đọc sự kiện");
            break;
        }
        if (bytes_read != sizeof(struct mouse_event)) {
            fprintf(stderr, "Dữ liệu đọc không đầy đủ\n");
            continue;
        }

        double timestamp = (double)event.timestamp_sec + (double)event.timestamp_nsec / 1e9;

        if (point_count >= MAX_POINTS) {
            printf("Trajectory quá dài, bỏ qua...\n");
            point_count = 0;
        }

        points[point_count].timestamp = timestamp;
        points[point_count].delta_x = event.x;
        points[point_count].delta_y = event.y;
        points[point_count].type = event.type;

        point_count++;

        if (event.type == 1 || event.type == 2) { // CLICK hoặc WHEEL
            if (point_count > 1) { // Đảm bảo có ít nhất 2 điểm
                process_trajectory(points, point_count);
            }
            point_count = 0;
        }
    }

    close(fd);
    return 0;
}