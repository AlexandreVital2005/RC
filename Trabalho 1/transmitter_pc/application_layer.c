#include "application_layer.h"
#include "protocol.h"
#include "link_layer.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

int get_file_size(const char *filename, long *size_out)
{
    struct stat st;
    if (stat(filename, &st) < 0) return -1;
    *size_out = st.st_size;
    return 0;
}

const char *get_basename(const char *path)
{
    const char *slash = strrchr(path, '/');
    return (slash == NULL) ? path : slash + 1;
}

int encode_file_size(unsigned char *out, long file_size)
{
    unsigned char temp[8];
    int count = 0;

    if (file_size == 0) {
        out[0] = 0;
        return 1;
    }

    while (file_size > 0) {
        temp[count++] = (unsigned char)(file_size & 0xFF);
        file_size >>= 8;
    }

    for (int i = 0; i < count; i++) {
        out[i] = temp[count - 1 - i];
    }

    return count;
}

int build_control_packet(unsigned char control,
                         const char *filename,
                         long file_size,
                         unsigned char *packet)
{
    unsigned char size_bytes[8];
    int size_len = encode_file_size(size_bytes, file_size);
    int name_len = (int)strlen(filename);
    int idx = 0;

    packet[idx++] = control;

    packet[idx++] = T_FILESIZE;
    packet[idx++] = size_len;
    memcpy(&packet[idx], size_bytes, size_len);
    idx += size_len;

    packet[idx++] = T_FILENAME;
    packet[idx++] = name_len;
    memcpy(&packet[idx], filename, name_len);
    idx += name_len;

    return idx;
}

int build_data_packet(const unsigned char *data,
                      int data_len,
                      unsigned char *packet)
{
    packet[0] = APP_DATA;
    packet[1] = data_len / 256;
    packet[2] = data_len % 256;
    memcpy(&packet[3], data, data_len);
    return data_len + 3;
}

int send_file(int fd, const char *filepath)
{
    FILE *f = fopen(filepath, "rb");
    if (f == NULL) {
        perror("fopen");
        return -1;
    }

    long file_size;
    if (get_file_size(filepath, &file_size) < 0) {
        perror("stat");
        fclose(f);
        return -1;
    }

    const char *filename = get_basename(filepath);

    unsigned char packet[MAX_FRAME_SIZE];
    unsigned char chunk[DATA_CHUNK_SIZE];

    int packet_len = build_control_packet(APP_START, filename, file_size, packet);
    printf("Sending START packet for file %s (%ld bytes)\n", filename, file_size);
    if (llwrite(fd, packet, packet_len) < 0) {
        fclose(f);
        return -1;
    }

    size_t nread;
    while ((nread = fread(chunk, 1, DATA_CHUNK_SIZE, f)) > 0) {
        packet_len = build_data_packet(chunk, (int)nread, packet);
        printf("Sending DATA packet with %zu bytes\n", nread);
        if (llwrite(fd, packet, packet_len) < 0) {
            fclose(f);
            return -1;
        }
    }

    if (ferror(f)) {
        perror("fread");
        fclose(f);
        return -1;
    }

    packet_len = build_control_packet(APP_END, filename, file_size, packet);
    printf("Sending END packet\n");
    if (llwrite(fd, packet, packet_len) < 0) {
        fclose(f);
        return -1;
    }

    fclose(f);
    return 0;
}
