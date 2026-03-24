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

volatile int STOP = FALSE;

int main(int argc, char *argv[])
{
    unsigned char Aread,Cread,Cant;
    unsigned char F=0x7E,A=0x03,C=0x03,BCC1=A^C,BCC2;

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

    unsigned char buf=0;
    unsigned char data[BUF_SIZE];

int bytes_read;
int i=0;

while(currentState!=STOPA){

    switch(currentState){

        case START:
            Cant=Cread;
            printf("Current State: START\n");
            bytes_read = read(fd, &buf, 1);
            if (bytes_read==1 && buf==F) {
                currentState=FLAG;
                printf("FLAG = 0x%02X\n", buf); 
            }

        break;

        case FLAG:
            printf("Current State: FLAG\n");
            bytes_read = read(fd, &buf, 1);
            if (bytes_read==1){
                Aread=buf;
                currentState=A_READ;
                printf("A = 0x%02X\n", Aread); 
            }


            else if (bytes_read==1 && buf!=A && buf!=F){
                currentState=START;
            }

        break;

        case A_READ:
            printf("Current State: A_READ\n");
            bytes_read = read(fd, &buf, 1);
            if (bytes_read==1){
                Cread=buf;
                currentState=C_READ;
                
                printf("C = 0x%02X\n", Cread); 
            }

            else if (bytes_read==1 && buf==F){
                currentState=FLAG;

            }

            else if (bytes_read==1 && buf!=C && buf!=F){
                currentState=START;
            }

        break;

        case C_READ:
            printf("Current State: C_READ\n");
            bytes_read = read(fd,&buf,1);

            if (bytes_read==1 && buf==(Aread^Cread)){
                currentState=DATA_READ;
                printf("BCC = 0x%02X\n", buf); 
            }

            else if (bytes_read==1 && buf==F){
                currentState=FLAG;
            }       

            else if (bytes_read==1 && buf!=(Aread^Cread) && buf!=F){
                currentState=START;
            }

        break;


        case DATA_READ:
            bytes_read = read(fd,&buf,1);
            if (bytes_read == 1 && buf==F){
                currentState=STOPA;
                i--;
            }
            
            else {
                data[i++]=buf;
            }

        break;

      }

    }
    printf("%d\n",i);
    printf("%d\n",data[0]);
    unsigned char BCC2_TEST=data[0];


    for (int j=1;j<i;j++){
        printf("%d\n",data[j]);
        BCC2_TEST ^= data[j]+1;
    }

    printf("%d\n",BCC2_TEST);
    printf("%d\n",data[i]);

    if (BCC2_TEST != data[i]){
        if (Cant != Cread){
            C = 0x01;
        }

        else if (Cant == Cread){
            C = 0x05;
        }
    }

    unsigned char buff[5] = {F,A,C,BCC1,F};
    write(fd, buff, 5);


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
