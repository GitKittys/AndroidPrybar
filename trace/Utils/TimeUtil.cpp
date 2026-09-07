//
// Created by ASUS on 2024-10-03.
//

#include "Utils.h"
#include <sys/time.h>
#include <ctime>

static inline void write2digit(char* p, int val) {
    p[0] = (char)('0' + val / 10);
    p[1] = (char)('0' + val % 10);
}
static inline void write3digit(char* p, int val) {
    p[0] = (char)('0' + val / 100);
    p[1] = (char)('0' + (val / 10) % 10);
    p[2] = (char)('0' + val % 10);
}

char* getCurrentTimeWithMilliseconds(char* buffer, size_t buffer_size) {
    struct timeval tv{};
    gettimeofday(&tv, nullptr);

    time_t now_time_t = tv.tv_sec;
    struct tm local_time{};
    localtime_r(&now_time_t, &local_time);

    // "[HH:MM:SS.mmm]" = 14 chars + null
    buffer[0] = '[';
    write2digit(buffer + 1, local_time.tm_hour);
    buffer[3] = ':';
    write2digit(buffer + 4, local_time.tm_min);
    buffer[6] = ':';
    write2digit(buffer + 7, local_time.tm_sec);
    buffer[9] = '.';
    write3digit(buffer + 10, (int)(tv.tv_usec / 1000));
    buffer[13] = ']';
    buffer[14] = '\0';

    return buffer;
}