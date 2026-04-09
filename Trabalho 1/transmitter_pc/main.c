#include <stdio.h>
#include <stdlib.h>

#include "link_layer.h"
#include "application_layer.h"

int main(int argc, char *argv[])
{
    if (argc != 3) {
        printf("Uso: %s <SerialPort> <Ficheiro>\n", argv[0]);
        printf("Exemplo: %s /dev/ttyS1 penguin.gif\n", argv[0]);
        return 1;
    }

    int fd = llopen(argv[1]);
    if (fd < 0) {
        printf("Erro no llopen\n");
        return 1;
    }

    if (send_file(fd, argv[2]) < 0) {
        printf("Erro a enviar ficheiro %s\n", argv[2]);
        llclose(fd);
        return 1;
    }

    if (llclose(fd) < 0) {
        printf("Erro no llclose\n");
        return 1;
    }

    printf("Transferencia concluida com sucesso\n");
    return 0;
}
