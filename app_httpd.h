#ifndef APP_HTTPD_H
#define APP_HTTPD_H

#include <Arduino.h>

enum Proto {
    PROTO_UART = 1,
    PROTO_I2C  = 2,
    PROTO_SPI  = 3,
};

void initCloudRelay(const char* host, uint16_t port, const char* path, bool ssl);

#endif