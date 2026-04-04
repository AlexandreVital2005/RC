#ifndef APPLICATION_LAYER_H
#define APPLICATION_LAYER_H

#include "protocol.h"

#define APP_DATA  0x01
#define APP_START 0x02
#define APP_END   0x03

#define T_FILESIZE 0x00
#define T_FILENAME 0x01

long decode_file_size(const unsigned char *bytes, int len);

int parse_control_packet(const unsigned char *packet,int packet_len,char *filename_out,int filename_max,long *file_size_out);

int handle_app_packet(const unsigned char *packet,int packet_len,AppReceiverContext *ctx);

#endif