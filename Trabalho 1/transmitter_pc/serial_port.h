#ifndef SERIAL_PORT_H
#define SERIAL_PORT_H

#include <termios.h>

int open_serial_port(const char *serialPort);
int configure_serial_port(int fd, struct termios *oldtio);
int restore_serial_port(int fd, const struct termios *oldtio);
int close_serial_port(int fd);

#endif
