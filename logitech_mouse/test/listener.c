#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>
#include <unistd.h>
#include <time.h>
#include <string.h>

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

const char* get_event_type(int type) {
    switch (type) {
        case 0: return "MOVE";
        case 1: return "CLICK";
        case 2: return "WHEEL";
        default: return "UNKNOWN";
    }
}

const char* get_button_name(int button) {
    switch (button) {
        case 0: return "LEFT";
        case 1: return "RIGHT";
        case 2: return "MIDDLE";
        default: return "UNKNOWN";
    }
}

const char* get_action_name(int action) {
    switch (action) {
        case 0: return "RELEASE";
        case 1: return "PRESS";
        default: return "UNKNOWN";
    }
}

int main() {
    int fd = open("/dev/logitech_mouse", O_RDONLY);
    if (fd < 0) {
        perror("Failed to open device");
        return EXIT_FAILURE;
    }

    printf("Listening for mouse events...\n");

    while (1) {
        struct mouse_event event;
        ssize_t bytes_read = read(fd, &event, sizeof(event));
        
        if (bytes_read < 0) {
            perror("Read error");
            break;
        }
        
        if (bytes_read == 0) {
            continue;
        }

        // Chuyển timestamp thành định dạng dễ đọc
        char time_buf[64];
        struct tm tm_info;
        time_t sec = (time_t)event.timestamp_sec; // Chuyển đổi kiểu
        localtime_r(&sec, &tm_info);
        strftime(time_buf, sizeof(time_buf), "%Y-%m-%d %H:%M:%S", &tm_info);
        
        printf("[%s.%09ld] ", time_buf, event.timestamp_nsec);

        switch (event.type) {
            case 0: // MOVE
                printf("MOVE: x=%d, y=%d\n", event.x, event.y);
                break;
            case 1: // CLICK
                printf("CLICK: button=%s, action=%s\n", 
                       get_button_name(event.button), 
                       get_action_name(event.action));
                break;
            case 2: // WHEEL
                printf("WHEEL: value=%d\n", event.wheel_value);
                break;
            default:
                printf("UNKNOWN EVENT\n");
                break;
        }
    }

    close(fd);
    return EXIT_SUCCESS;
}