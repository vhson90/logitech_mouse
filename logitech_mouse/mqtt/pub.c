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
#define MAX_EVENTS  10000 // Tương tự MAX_POINTS trong mouse_listener.c
#define COSINE_TOLERANCE 0.98 // cos(11.5 độ) ~ 0.98
#define MIN_VECTOR_LENGTH 1.0 // Độ dài vector tối thiểu để tính accuracy

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

// Tính tốc độ và độ chính xác từ dữ liệu chuột
void calculate_speed_and_accuracy(struct mouse_event events[], int count, double* speed, double* accuracy) {
    if (count < 2) {
        *speed = 0.0;
        *accuracy = 0.0;
        return;
    }

    // Tổng thời gian bao gồm cả điểm cuối (CLICK hoặc WHEEL)
    double total_time = (double)(events[count - 1].timestamp_sec - events[0].timestamp_sec) +
                        (double)(events[count - 1].timestamp_nsec - events[0].timestamp_nsec) / 1e9;
    if (total_time < 1.0 || total_time > 10.0) {
        *speed = 0.0;
        *accuracy = 0.0;
        return;
    }

    // Tính tổng khoảng cách (chỉ giữa các MOVE)
    double total_distance = 0.0;
    for (int i = 0; i < count - 1; i++) {
        if (events[i].type == 0 && events[i + 1].type == 0) {
            double dx = (double)events[i + 1].x;
            double dy = (double)events[i + 1].y;
            total_distance += sqrt(dx * dx + dy * dy);
        }
    }

    *speed = total_distance / total_time;

    // Tính accuracy dựa trên cosine của góc giữa các đoạn MOVE
    int eqdir_count = 0;
    int valid_segments = 0;
    for (int i = 0; i < count - 2; i++) {
        if (events[i].type == 0 && events[i + 1].type == 0 && events[i + 2].type == 0) {
            double dx1 = (double)events[i + 1].x;
            double dy1 = (double)events[i + 1].y;
            double dx2 = (double)events[i + 2].x;
            double dy2 = (double)events[i + 2].y;

            // Tính độ dài vector
            double len1 = sqrt(dx1 * dx1 + dy1 * dy1);
            double len2 = sqrt(dx2 * dx2 + dy2 * dy2);

            // Chỉ tính nếu cả hai đoạn đủ dài
            if (len1 >= MIN_VECTOR_LENGTH && len2 >= MIN_VECTOR_LENGTH) {
                double dot_product = dx1 * dx2 + dy1 * dy2;
                double cos_theta = dot_product / (len1 * len2);

                if (cos_theta > 1.0) cos_theta = 1.0;
                if (cos_theta < -1.0) cos_theta = -1.0;

                if (cos_theta >= COSINE_TOLERANCE) {
                    eqdir_count++;
                }
                valid_segments++;
            }
        }
    }
    *accuracy = (valid_segments > 0) ? (double)eqdir_count / valid_segments : 1.0;
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

    struct mouse_event events[MAX_EVENTS];
    int event_count = 0;
    double trajectory_time = 0.0;

    printf("Bắt đầu theo dõi sự kiện chuột và gửi lên MQTT...\n");

    while (1) {
        struct mouse_event event;
        ssize_t bytes_read = read(fd, &event, sizeof(struct mouse_event));
        if (bytes_read != sizeof(struct mouse_event)) {
            usleep(10000); // Đợi 10ms nếu không có dữ liệu
            continue;
        }

        if (event_count >= MAX_EVENTS) {
            printf("Trajectory quá dài, bỏ qua...\n");
            event_count = 0;
            trajectory_time = 0.0;
        }

        events[event_count++] = event;

        // Tính thời gian quỹ đạo
        if (event_count > 1) {
            trajectory_time = (double)(event.timestamp_sec - events[0].timestamp_sec) +
                              (double)(event.timestamp_nsec - events[0].timestamp_nsec) / 1e9;
        }

        // Kết thúc quỹ đạo khi gặp CLICK/WHEEL hoặc thời gian vượt 10s
        if (event.type == 1 || event.type == 2 || trajectory_time > 10.0) {
            if (trajectory_time >= 1.0 && trajectory_time <= 10.0 && event_count > 1) { // Chỉ xét quỹ đạo từ 1-10s
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