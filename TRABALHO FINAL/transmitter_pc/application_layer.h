#ifndef APPLICATION_LAYER_H
#define APPLICATION_LAYER_H

int get_file_size(const char *filename, long *size_out);
const char *get_basename(const char *path);
int encode_file_size(unsigned char *out, long file_size);

int build_control_packet(unsigned char control,
                         const char *filename,
                         long file_size,
                         unsigned char *packet);

int build_data_packet(const unsigned char *data,
                      int data_len,
                      unsigned char *packet);

int send_file(int fd, const char *filepath);

#endif
