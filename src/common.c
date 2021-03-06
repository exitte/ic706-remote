/*
 * Copyright (c) 2014, Alexandru Csete
 * All rights reserved.
 *
 * This software is licensed under the terms and conditions of the
 * Simplified BSD License. See license.txt for details.
 *
 */
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>              /* O_WRONLY */
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/time.h>
#include <termios.h>
#include <unistd.h>

#include "common.h"

/* Print an array of chars as HEX numbers */
inline void print_buffer(int from, int to, const uint8_t * buf,
                         unsigned int len)
{
    unsigned int    i;

    fprintf(stderr, "%d -> %d:", from, to);

    for (i = 0; i < len; i++)
        fprintf(stderr, " %02X", buf[i]);

    fprintf(stderr, "\n");
}

/* Configure serial interface to raw mode with specified attributes */
int set_serial_config(int fd, int speed, int parity, int blocking)
{
    struct termios  tty;

    memset(&tty, 0, sizeof(tty));
    if (tcgetattr(fd, &tty) != 0)
    {
        fprintf(stderr, "error %d from tcgetattr", errno);
        return -1;
    }

    cfsetospeed(&tty, speed);
    cfsetispeed(&tty, speed);

    tty.c_cflag = (tty.c_cflag & ~CSIZE) | CS8; // 8-bit chars
    // disable IGNBRK for mismatched speed tests; otherwise receive break
    // as \000 chars
    tty.c_iflag &= ~IGNBRK;     // disable break processing
    tty.c_lflag = 0;            // no signaling chars, no echo,
    // no canonical processing

    /* no remapping, no delays */
    tty.c_oflag = 0;

    /* 0.5 sec read timeout */
    tty.c_cc[VMIN] = blocking ? 1 : 0;
    tty.c_cc[VTIME] = 5;

    /* shut off xon/xoff ctrl */
    tty.c_iflag &= ~(IXON | IXOFF | IXANY);

    /* ignore modem controls and enable reading */
    tty.c_cflag |= (CLOCAL | CREAD);

    /* parity */
    tty.c_cflag &= ~(PARENB | PARODD);
    tty.c_cflag |= parity;
    tty.c_cflag &= ~CSTOPB;
    tty.c_cflag &= ~CRTSCTS;

    if (tcsetattr(fd, TCSANOW, &tty) != 0)
    {
        fprintf(stderr, "error %d from tcsetattr", errno);
        return -1;
    }

    return 0;
}

int create_server_socket(int port)
{
    struct sockaddr_in serv_addr;
    int             sock_fd = -1;
    int             yes = 1;

    sock_fd = socket(AF_INET, SOCK_STREAM, 0);
    if (sock_fd == -1)
    {
        fprintf(stderr, "Error creating socket: %d: %s\n", errno,
                strerror(errno));

        return -1;
    }

    if (setsockopt(sock_fd, SOL_SOCKET, SO_REUSEADDR, &yes, sizeof(yes)) == -1)
        fprintf(stderr, "Error setting SO_REUSEADDR: %d: %s\n", errno,
                strerror(errno));

    /* bind socket to host address */
    memset(&serv_addr, 0, sizeof(struct sockaddr_in));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(port);
    if (bind(sock_fd, (struct sockaddr *)&serv_addr, sizeof(serv_addr)) == -1)
    {
        fprintf(stderr, "bind() error: %d: %s\n", errno, strerror(errno));

        close(sock_fd);
        return -1;
    }

    if (listen(sock_fd, 1) == -1)
    {
        fprintf(stderr, "listen() error: %d: %s\n", errno, strerror(errno));

        close(sock_fd);
        return -1;
    }

    return sock_fd;
}

int read_data(int fd, struct xfr_buf *buffer)
{
    uint8_t        *buf = buffer->data;
    int             type = PKT_TYPE_INCOMPLETE;
    size_t          num;

    /* read data */
    num = read(fd, &buf[buffer->wridx], RDBUF_SIZE - buffer->wridx);

    if (num > 0)
    {
        buffer->wridx += num;

        /* There is at least one character in the buffer.
         *
         * If buf[0] = 0xFE then this is a regular packet. Check if
         * buf[end] = 0xFD, if yes, the packet is complete and return
         * the packet type.
         *
         * If buf[0] = 0x00 and wridx = 1 then this is an EOS packet.
         * If buf[0] = 0x00 and wridx > 1 then this is an invalid
         * packet (does not start with 0xFE).
         */
        if (buf[0] == 0xFE)
        {
            if (buf[buffer->wridx - 1] == 0xFD)
                type = buf[1];
            else
                type = PKT_TYPE_INCOMPLETE;
        }
        else if ((buf[0] == 0x00) && (buffer->wridx == 1))
        {
            type = PKT_TYPE_EOS;
        }
        else
        {
            type = PKT_TYPE_INVALID;
        }
    }
    else if (num == 0)
    {
        type = PKT_TYPE_EOF;
        fprintf(stderr, "Received EOF from FD %d\n", fd);
    }
    else
    {
        type = PKT_TYPE_INVALID;
        fprintf(stderr, "Error reading from FD %d: %d: %s\n", fd, errno,
                strerror(errno));
    }

    return type;
}

int transfer_data(int ifd, int ofd, struct xfr_buf *buffer)
{
    uint8_t         init1_resp[] = { 0xFE, 0xF0, 0xFD };
    uint8_t         init2_resp[] = { 0xFE, 0xF1, 0xFD };
    int             pkt_type;

    pkt_type = read_data(ifd, buffer);
    switch (pkt_type)
    {
    case PKT_TYPE_KEEPALIVE:
        /* emulated on server side; do not forward */
        buffer->wridx = 0;
        buffer->valid_pkts++;

    case PKT_TYPE_INIT1:
        /* Sent by the first unit that is powered on.
           Expects PKT_TYPE_INIT1 + PKT_TYPE_INIT2 in response. */
        buffer->write_errors += write(ifd, init1_resp, 3) != 3;
        buffer->write_errors += write(ifd, init2_resp, 3) != 3;
        buffer->wridx = 0;
        buffer->valid_pkts++;
        break;

    case PKT_TYPE_INIT2:
        /* Sent by the panel when powered on and the radio is already on.
           Expects PKT_TYPE_INIT2 in response. */
        buffer->write_errors += write(ifd, init2_resp, 3);
        buffer->wridx = 0;
        buffer->valid_pkts++;
        break;

    case PKT_TYPE_PWK:
        /* Power on/off message sent by panel; leave handling to server */
#if DEBUG
        print_buffer(ifd, ofd, buffer->data, buffer->wridx);
#endif
        buffer->wridx = 0;
        buffer->valid_pkts++;
        break;

    case PKT_TYPE_INCOMPLETE:
        break;

    case PKT_TYPE_INVALID:
        buffer->invalid_pkts++;
        buffer->wridx = 0;
        break;

    default:
        /* we also "send" on EOF packet because buffer may not be empty */
#if DEBUG
        print_buffer(ifd, ofd, buffer->data, buffer->wridx);
#endif
        buffer->write_errors +=
            write(ofd, buffer->data, buffer->wridx) != buffer->wridx;

        buffer->wridx = 0;
        buffer->valid_pkts++;
    }

    return pkt_type;
}

uint64_t time_ms(void)
{
    struct timeval  tval;

    gettimeofday(&tval, NULL);

    return 1e3 * tval.tv_sec + 1e-3 * tval.tv_usec;
}

uint64_t time_us(void)
{
    struct timeval  tval;

    gettimeofday(&tval, NULL);

    return 1e6 * tval.tv_sec + tval.tv_usec;
}

int send_keepalive(int fd)
{
    char            msg[] = { 0xFE, 0x0B, 0x00, 0xFD };

    return (write(fd, msg, 4) != 4);
}

void send_pwr_message(int fd, int poweron)
{
    char            msg[] = { 0xFE, 0xA0, 0x00, 0xFD };

    if (poweron)
        msg[2] = 0x01;

    if (write(fd, msg, 4) == -1)
        fprintf(stderr, "Error sending PWR message %d (%s)\n", errno,
                strerror(errno));
}

int pwk_init(void)
{
    int             fd;
    int             wr_err = 0;


    /*  Export GPIO7 unless it is already exported */
    if (access("/sys/class/gpio/gpio7", F_OK) == -1)
    {
        /*  $ echo 7 > /sys/class/gpio/export */
        fd = open("/sys/class/gpio/export", O_WRONLY);
        if (fd < 0)
            return -1;

        wr_err += write(fd, "7", 1) != 1;
        close(fd);
    }

    /*  $ echo "in" > /sys/class/gpio/gpio7/direction */
    fd = open("/sys/class/gpio/gpio7/direction", O_WRONLY);
    if (fd < 0)
        return -1;

    wr_err += write(fd, "in", 2) != 2;
    close(fd);

    /*  $ echo 1 > /sys/class/gpio/gpio7/active_low */
    fd = open("/sys/class/gpio/gpio7/active_low", O_WRONLY);
    if (fd < 0)
        return -1;

    wr_err += write(fd, "1", 1) != 1;
    close(fd);

    /*  $ echo "falling" > /sys/class/gpio/gpio7/edge  */
    fd = open("/sys/class/gpio/gpio7/edge", O_WRONLY);
    if (fd < 0)
        return -1;

    wr_err += write(fd, "falling", 7) != 7;
    close(fd);

    fd = open("/sys/class/gpio/gpio7/value", O_RDONLY);

    if (wr_err)
        fprintf(stderr, "Write errors during PWK_INIT: %d\n", wr_err);

    return fd;
}


#define SYSFS_GPIO_DIR "/sys/class/gpio/"
#define MAX_GPIO_BUF   100

int gpio_init_out(unsigned int gpio)
{
    int             fd;
    int             len;
    int             wr_err = 0;
    char            buf[MAX_GPIO_BUF];


    /* check whether GPIO is already exported, if not, export it */
    snprintf(buf, sizeof(buf), SYSFS_GPIO_DIR "gpio%d", gpio);
    if (access(buf, F_OK) == -1)
    {
        /* export GPIO */
        fd = open(SYSFS_GPIO_DIR "export", O_WRONLY);
        if (fd < 0)
            return -1;

        len = snprintf(buf, sizeof(buf), "%d", gpio);
        wr_err += write(fd, buf, len) != len;
        close(fd);
    }

    /* set direction to "out" */
    snprintf(buf, sizeof(buf), SYSFS_GPIO_DIR "gpio%d/direction", gpio);
    fd = open(buf, O_WRONLY);
    if (fd < 0)
        return -1;

    wr_err += write(fd, "out", 3) != 3;
    close(fd);

    /* intialize with a 0 */
    if (gpio_set_value(20, 0) < 0)
        return -1;

    if (wr_err)
    {
        fprintf(stderr, "Write errors during GPIO init out: %d\n", wr_err);
        return -1;
    }

    return 0;
}

int gpio_set_value(unsigned int gpio, unsigned int value)
{
    int             fd;
    int             wr_err = 0;
    char            buf[MAX_GPIO_BUF];

    snprintf(buf, sizeof(buf), SYSFS_GPIO_DIR "gpio%d/value", gpio);
    fd = open(buf, O_WRONLY);
    if (fd < 0)
        return -1;

    if (value == 1)
        wr_err += write(fd, "1", 1) != 1;
    else
        wr_err += write(fd, "0", 1) != 1;

    close(fd);

    return -wr_err;
}
