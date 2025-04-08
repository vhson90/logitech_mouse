#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <fcntl.h>
#include <math.h>
#include <MQTTClient.h>

/*
Broker: broker.emqx.io
TCP Port: 1883 
*/
#define ADDRESS     "tcp://broker.emqx.io:1883"
#define CLIENTID    "publisher_mouse_driver"
#define PUB_TOPIC   "mouse_driver/speed_and_accuracy"
#define DEVICE_PATH "/dev/logitech_mouse"

// Cấu trúc dữ liệu sự kiện chuột từ driver
struct mouse_event {
    long long timestamp_sec; // Giây
    long timestamp_nsec;     // Nano giây
    int type;                // 0: MOVE, 1: CLICK, 2: WHEEL
    int x;                   // Tọa độ x
    int y;                   // Tọa độ y
    int button;              // 0: LEFT, 1: RIGHT, 2: MIDDLE
    int action;              // 0: RELEASE, 1: PRESS
    int wheel_value;         // Giá trị cuộn
};

// Hàm gửi dữ liệu lên MQTT
void publish(MQTTClient client, char* topic, char* payload) {
    MQTTClient_message pubmsg = MQTTClient_message_initializer;
    pubmsg.payload = payload;
    pubmsg.payloadlen = strlen(pubmsg.payload);
    pubmsg.qos = 1;
    pubmsg.retained = 0;
    MQTTClient_deliveryToken token;
    MQTTClient_publishMessage(client, topic, &pubmsg, &token);
    MQTTClient_waitForCompletion(client, token, 1000L);
    printf("Message '%s' with delivery token %d delivered\n", payload, token);
}

// Tính khoảng cách Euclidean giữa hai điểm
double calculate_distance(int x1, int y1, int x2, int y2) {
    return sqrt(pow(x2 - x1, 2) + pow(y2 - y1, 2));
}

// Tính tốc độ và độ chính xác từ dữ liệu chuột
void calculate_speed_and_accuracy(struct mouse_event events[], int count, double* speed, double* accuracy) {
    double total_distance = 0.0;
    int direction_changes = 0;
    int valid_moves = 0;

    if (count < 2) {
        *speed = 0.0;
        *accuracy = 0.0;
        return;
    }

    // Tính tổng khoảng cách và thời gian
    for (int i = 1; i < count; i++) {
        if (events[i].type == 0 && events[i-1].type == 0) { // Chỉ xét MOVE events
            total_distance += calculate_distance(events[i-1].x, events[i-1].y, events[i].x, events[i].y);
            valid_moves++;

            // Kiểm tra thay đổi hướng
            int dx1 = events[i].x - events[i-1].x;
            int dy1 = events[i].y - events[i-1].y;
            if (i > 1) {
                int dx0 = events[i-1].x - events[i-2].x;
                int dy0 = events[i-1].y - events[i-2].y;
                if ((dx1 * dx0 < 0 || dy1 * dy0 < 0) && (dx1 != 0 || dy1 != 0)) {
                    direction_changes++;
                }
            }
        }
    }

    // Tính thời gian (giây) giữa sự kiện đầu và cuối
    double time_diff = (events[count-1].timestamp_sec - events[0].timestamp_sec) +
                      (events[count-1].timestamp_nsec - events[0].timestamp_nsec) / 1e9;

    // Tính tốc độ
    *speed = (time_diff > 0 && valid_moves > 0) ? total_distance / time_diff : 0.0;

    // Tính độ chính xác
    *accuracy = (valid_moves > 1) ? 1.0 - (double)direction_changes / (valid_moves - 1) : 1.0;
}

int main(int argc, char* argv[]) {
    MQTTClient client;
    MQTTClient_create(&client, ADDRESS, CLIENTID, MQTTCLIENT_PERSISTENCE_NONE, NULL);
    MQTTClient_connectOptions conn_opts = MQTTClient_connectOptions_initializer;

    int rc;
    if ((rc = MQTTClient_connect(client, &conn_opts)) != MQTTCLIENT_SUCCESS) {
        printf("Failed to connect, return code %d\n", rc);
        exit(-1);
    }

    // Mở file thiết bị chuột
    int fd = open(DEVICE_PATH, O_RDONLY);
    if (fd < 0) {
        printf("Failed to open device %s\n", DEVICE_PATH);
        MQTTClient_disconnect(client, 1000);
        MQTTClient_destroy(&client);
        exit(-1);
    }

    struct mouse_event events[100]; // Buffer lưu tối đa 100 sự kiện
    int event_count = 0;
    double trajectory_time = 0.0;

    while (1) {
        struct mouse_event event;
        ssize_t bytes_read = read(fd, &event, sizeof(struct mouse_event));
        if (bytes_read != sizeof(struct mouse_event)) {
            usleep(10000); // Đợi 10ms nếu không có dữ liệu
            continue;
        }

        events[event_count++] = event;

        // Tính thời gian quỹ đạo
        if (event_count > 1) {
            trajectory_time = (event.timestamp_sec - events[0].timestamp_sec) +
                             (event.timestamp_nsec - events[0].timestamp_nsec) / 1e9;
        }

        // Kết thúc quỹ đạo khi gặp CLICK/WHEEL hoặc thời gian vượt 10s
        if (event.type != 0 || trajectory_time > 10.0 || event_count >= 100) {
            if (trajectory_time >= 1.0 && event_count > 1) { // Chỉ xét quỹ đạo từ 1-10s
                double speed, accuracy;
                calculate_speed_and_accuracy(events, event_count, &speed, &accuracy);

                // Tạo payload JSON
                char payload[256];
                snprintf(payload, sizeof(payload), "{\"speed\": %.2f, \"accuracy\": %.2f}", speed, accuracy);
                publish(client, PUB_TOPIC, payload);
            }
            event_count = 0; // Reset buffer
            trajectory_time = 0.0;
        }
    }

    close(fd);
    MQTTClient_disconnect(client, 1000);
    MQTTClient_destroy(&client);
    return 0;
}