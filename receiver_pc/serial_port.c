#include "serial_port.h"

#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#define BAUDRATE B38400

int open_serial_port(const char *serialPort)
{
    int fd = open(serialPort, O_RDWR | O_NOCTTY);
    if (fd < 0) {
        perror(serialPort);
        return -1;
    }

    return fd;
}

int configure_serial_port(int fd, struct termios *oldtio)
{
    if (tcgetattr(fd, oldtio) == -1) {
        perror("tcgetattr");
        return -1;
    }

    struct termios newtio;
    memset(&newtio, 0, sizeof(newtio));

    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0;
    newtio.c_cc[VMIN] = 1;

    tcflush(fd, TCIOFLUSH);

    if (tcsetattr(fd, TCSANOW, &newtio) == -1) {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

int restore_serial_port(int fd, const struct termios *oldtio)
{
    if (tcsetattr(fd, TCSANOW, oldtio) == -1) {
        perror("tcsetattr");
        return -1;
    }

    return 0;
}

int close_serial_port(int fd)
{
    if (close(fd) == -1) {
        perror("close");
        return -1;
    }

    return 0;
}