//
// Created by ASUS on 2025-11-30.
//

#ifndef UNICRONTEST_HEXDUMP_H
#define UNICRONTEST_HEXDUMP_H


#include <cstddef>
#include <cstdint>

size_t hexdump_to_buffer(
        uint64_t addr,
        const void *buf,
        size_t size,
        char *outbuf,
        size_t outcap);

#endif //UNICRONTEST_HEXDUMP_H
