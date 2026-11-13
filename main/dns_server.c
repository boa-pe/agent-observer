#include "dns_server.h"
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "esp_log.h"

static const char *TAG = "dns";

static void dns_task(void *arg)
{
    int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock < 0) {
        ESP_LOGE(TAG, "socket failed");
        vTaskDelete(NULL);
        return;
    }
    struct sockaddr_in addr = {
        .sin_family = AF_INET,
        .sin_port = htons(53),
        .sin_addr.s_addr = htonl(INADDR_ANY),
    };
    if (bind(sock, (struct sockaddr *)&addr, sizeof(addr)) < 0) {
        ESP_LOGE(TAG, "bind failed");
        close(sock);
        vTaskDelete(NULL);
        return;
    }
    static uint8_t buf[512];
    for (;;) {
        struct sockaddr_in from;
        socklen_t flen = sizeof(from);
        int n = recvfrom(sock, buf, sizeof(buf) - 16, 0, (struct sockaddr *)&from, &flen);
        if (n < 12) continue;
        /* only standard queries with exactly one question */
        if ((buf[2] & 0x80) || buf[4] != 0 || buf[5] != 1) continue;
        /* walk the QNAME */
        int p = 12;
        while (p < n && buf[p]) p += buf[p] + 1;
        p += 5; /* zero byte + QTYPE + QCLASS */
        if (p > n) continue;
        uint16_t qtype = (buf[p - 4] << 8) | buf[p - 3];

        buf[2] = 0x84 | (buf[2] & 0x01); /* response, authoritative, keep RD */
        buf[3] = 0x80;                   /* RA, no error */
        buf[6] = 0; buf[7] = (qtype == 1) ? 1 : 0; /* ANCOUNT */
        buf[8] = buf[9] = buf[10] = buf[11] = 0;
        int len = p;
        if (qtype == 1) {
            static const uint8_t ans[] = {
                0xC0, 0x0C,             /* pointer to name */
                0x00, 0x01, 0x00, 0x01, /* A, IN */
                0x00, 0x00, 0x00, 0x3C, /* TTL 60 */
                0x00, 0x04, 192, 168, 4, 1,
            };
            memcpy(buf + p, ans, sizeof(ans));
            len += sizeof(ans);
        }
        sendto(sock, buf, len, 0, (struct sockaddr *)&from, flen);
    }
}

void dns_server_start(void)
{
    xTaskCreate(dns_task, "dns", 3072, NULL, 5, NULL);
}
