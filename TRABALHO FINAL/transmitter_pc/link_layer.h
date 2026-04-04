#ifndef LINK_LAYER_H
#define LINK_LAYER_H

int llopen(const char *serialPort);
int llwrite(int fd, const unsigned char *buffer, int length);
int llclose(int fd);

#endif
