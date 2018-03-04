// Read analog values from Linux IIO device
//
// Copyright (C) 2017  Kevin O'Connor <kevin@koconnor.net>
//
// This file may be distributed under the terms of the GNU GPLv3 license.

#include <fcntl.h> // open
#include <stdio.h> // snprintf
#include <stdlib.h> // atoi
#include <unistd.h> // read
#include "command.h" // shutdown
#include "gpio.h" // gpio_adc_setup
#include "internal.h" // report_errno
#include "sched.h" // sched_shutdown
#include "generic/spi.h"

DECL_CONSTANT(ADC_MAX, 4095); // Assume 12bit adc

#define IIO_PATH "/sys/bus/iio/devices/iio:device0/in_voltage%d_raw"

struct gpio_adc
gpio_adc_setup(uint8_t pin)
{
    char fname[256];
    snprintf(fname, sizeof(fname), IIO_PATH, pin);

    int fd = open(fname, O_RDONLY|O_CLOEXEC);
    if (fd < 0) {
        report_errno("analog open", fd);
        goto fail;
    }
    int ret = set_non_blocking(fd);
    if (ret < 0)
        goto fail;
    return (struct gpio_adc){ .fd = fd };
fail:
    if (fd >= 0)
        close(fd);
    shutdown("Unable to open adc device");
}

uint32_t
gpio_adc_sample(struct gpio_adc g)
{
    return 0;
}

uint16_t
gpio_adc_read(struct gpio_adc g)
{
    char buf[64];
    int ret = pread(g.fd, buf, sizeof(buf)-1, 0);
    if (ret <= 0) {
        report_errno("analog read", ret);
        try_shutdown("Error on analog read");
        return 0;
    }
    buf[ret] = '\0';
    return atoi(buf);
}

void
gpio_adc_cancel_sample(struct gpio_adc g)
{
}

SPI_t spi_basic_config = 0;

void spi_init(void) {
    spi_basic_config = spi_get_config(0, 4000000);
}
DECL_INIT(spi_init);

SPI_t spi_get_config(uint8_t const mode, uint32_t const speed) {
    return spi_basic_config;
}
void spi_set_config(SPI_t const config) {
}
void spi_transfer_len(char *data, uint8_t len) {
    (void)data; (void)len;
}
uint8_t spi_transfer(uint8_t const data, uint8_t const last) {
    return data;
}
