// Alexandre Vital e Francisco Nunes
// Read from serial port in non-canonical mode
//
// Modified by: Eduardo Nuno Almeida [enalmeida@fe.up.pt]

#include <fcntl.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <termios.h>
#include <unistd.h>

typedef enum{
    START,
    FLAG,
    A_READ,
    C_READ,
    DATA_READ,
    STOPA,
} stateNames;

stateNames currentState=START;

// Baudrate settings are defined in <asm/termbits.h>, which is
// included by <termios.h>
#define BAUDRATE B38400
#define _POSIX_SOURCE 1 // POSIX compliant source

#define FALSE 0
#define TRUE 1

#define BUF_SIZE 9

#define C_SET   0x03
#define C_UA    0x07
#define C_DISC  0x0B
#define C_I0    0x00
#define C_I1    0x40

volatile int STOP = FALSE;

unsigned char rr_for(int nr) {
    return (nr == 0) ? 0x05 : 0x85;
}

unsigned char rej_for(int nr) {
    return (nr == 0) ? 0x01 : 0x81;
}

int main(int argc, char *argv[])
{
    unsigned char Aread=0,Cread=0;
    unsigned char F=0x7E,A=0x03,BCC2_teste=0,buf=0;

    // Program usage: Uses either COM1 or COM2
    const char *serialPortName = argv[1];

    if (argc < 2)
    {
        printf("Incorrect program usage\n"
               "Usage: %s <SerialPort>\n"
               "Example: %s /dev/ttyS1\n",
               argv[0],
               argv[0]);
        exit(1);
    }

    // Open serial port device for reading and writing and not as controlling tty
    // because we don't want to get killed if linenoise sends CTRL-C.
    int fd = open(serialPortName, O_RDWR | O_NOCTTY);
    if (fd < 0)
    {
        perror(serialPortName);
        exit(-1);
    }

    struct termios oldtio;
    struct termios newtio;

    // Save current port settings
    if (tcgetattr(fd, &oldtio) == -1)
    {
        perror("tcgetattr");
        exit(-1);
    }

    // Clear struct for new port settings
    memset(&newtio, 0, sizeof(newtio));

    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;

    // Set input mode (non-canonical, no echo,...)
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0; // Inter-character timer unused
    newtio.c_cc[VMIN] = 1;  // Blocking read until 5 chars received

    // VTIME e VMIN should be changed in order to protect with a
    // timeout the reception of the following character(s)

    // Now clean the line and activate the settings for the port
    // tcflush() discards data written to the object referred to
    // by fd but not transmitted, or data received but not read,
    // depending on the value of queue_selector:
    //   TCIFLUSH - flushes data received but not read.
    tcflush(fd, TCIOFLUSH);

    // Set new port settings
    if (tcsetattr(fd, TCSANOW, &newtio) == -1)
    {
        perror("tcsetattr");
        exit(-1);
    }

    printf("New termios structure set\n");

    unsigned char data[BUF_SIZE];
    unsigned char reply[5];

    int bytes_read;
    int i=0;
    int NS_esperado = 0;

while(currentState!=STOPA){

    switch(currentState){

        case START:

            printf("Current State: START\n");
            i = 0;
            bytes_read = read(fd, &buf, 1);

            if (bytes_read==1 && buf==F) {
                currentState=FLAG;
                printf("FLAG = 0x%02X\n", buf); 
            }

        break;

        case FLAG:

            printf("Current State: FLAG\n");
            bytes_read = read(fd, &buf, 1);

            if (bytes_read == 1 && buf == F) {
                currentState = FLAG;
            }

            else if (bytes_read == 1 && buf == A) {
                Aread = buf;
                currentState = A_READ;
                printf("A = 0x%02X\n", Aread);
            }

            else if (bytes_read == 1) {
                currentState = START;
            }
        break;

        case A_READ:

            printf("Current State: A_READ\n");
            bytes_read = read(fd, &buf, 1);

            if (bytes_read == 1) {
                printf("Byte lido em A_READ = 0x%02X\n", buf);
            }

            if (bytes_read == 1 && buf == F) {
                currentState = FLAG;
            }

            else if (bytes_read == 1 && (buf == C_I0 || buf == C_I1 || buf == C_SET || buf == C_DISC || buf == C_UA)) {
                Cread = buf;
                currentState = C_READ;
                printf("C = 0x%02X\n", Cread);
            }

            else if (bytes_read == 1) {
                currentState = START;
            }

        break;

        case C_READ:

            printf("Current State: C_READ\n");
            bytes_read = read(fd,&buf,1);

            if (bytes_read == 1 && buf == (Aread^Cread)){
                currentState=DATA_READ;
                printf("BCC = 0x%02X\n", buf); 
            }

            else if (bytes_read == 1 && buf == F){
                currentState=FLAG;
            }       

            else if (bytes_read == 1){
                currentState=START;
            }

        break;


        case DATA_READ:

            bytes_read = read(fd, &buf, 1);

            if (bytes_read == 1 && buf == F) {
                // End of frame reached
                if (i < 1) {
                    // no BCC2/data
                    printf("Frame too short\n");
                    currentState = START;
                    i = 0;
                    break;
                }

                int NS_recebido = (Cread == 0x40) ? 1 : 0;
                int tamanhoData = i - 1; // last byte stored is BCC2
                unsigned char BCC2_recebido = data[i - 1];

                printf("NS recebido = %d, expectedNs = %d\n", NS_recebido, NS_esperado);

                // Prepare reply frame skeleton: F A C BCC1 F
                reply[0] = F;
                reply[1] = A;   // 0x03 for receiver reply
                reply[4] = F;

                if (tamanhoData < 0) {
                    currentState = START;
                    i = 0;
                    break;
                }

                // If there is payload, compute BCC2
                if (tamanhoData > 0) {
                    BCC2_teste = data[0];
                    for (int j = 1; j < tamanhoData; j++) {
                        BCC2_teste ^= data[j];
                    }

                } else {
                    // edge case: empty payload
                    BCC2_teste = 0x00;
                }

                    printf("BCC2 calculado = 0x%02X\n", BCC2_teste);
                    printf("BCC2 recebido  = 0x%02X\n", BCC2_recebido);

                if (NS_recebido == NS_esperado) {
                    // New frame
                    if (BCC2_teste == BCC2_recebido) {
                        // New and correct -> accept, send RR(nextNs)
                        printf("New correct frame received\n");

                        // Here you would pass data[0..data_len-1] to the application
                        printf("Payload length = %d\n", tamanhoData);
                        printf("Payload bytes: ");

                        for (int j = 0; j < tamanhoData; j++) {
                            printf("0x%02X ", data[j]);
                        }

                        printf("\n");

                        NS_esperado = 1 - NS_esperado;

                        reply[2] = rr_for(NS_esperado);
                        reply[3] = reply[1] ^ reply[2];

                        write(fd, reply, 5);

                        // For testing a single frame, stop here.
                        // If you want continuous reception, change to START.
                        currentState = STOPA;
                        }

                    else {
                        // New but with BCC2 error -> REJ(expectedNs)
                        printf("New frame with BCC2 error\n");

                        reply[2] = rej_for(NS_esperado);
                        reply[3] = reply[1] ^ reply[2];

                        write(fd, reply, 5);

                        currentState = START;
                    }
                }

                else {
                    // Duplicate frame -> discard and send RR(expectedNs)
                    printf("Duplicate frame received\n");

                    reply[2] = rr_for(NS_esperado);
                    reply[3] = reply[1] ^ reply[2];

                    write(fd, reply, 5);

                    currentState = START;
                    }

                i = 0;
            }

            else if (bytes_read == 1) {
                if (i < BUF_SIZE) {
                     data[i++] = buf;
                }

                else {
                    // overflow
                    printf("Buffer overflow\n");
                    i = 0;
                    currentState = START;
                    }
                }
        break;

      }

    }

    



  sleep(1);

    // The while() cycle should be changed in order to respect the specifications
    // of the protocol indicated in the Lab guide

    // Restore the old port settings
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
    {
        perror("tcsetattr");
        exit(-1);
    }

    close(fd);

    return 0;
}
