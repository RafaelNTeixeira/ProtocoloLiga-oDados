// Link layer protocol implementation

#include "link_layer.h"

// MISC
#define _POSIX_SOURCE 1 // POSIX compliant source

extern int alarmCount;

struct termios oldtio;

int connectionType;
int tries;
int timeout;
int sendReceiveValidate;
int fullLength = 0; // troquei totalBufferSize por fullLength
u_int8_t responseByte;
unsigned char frameNumberByte = 0x00;

extern int alarmEnabled;

////////////////////////////////////////////////
// LLOPEN
////////////////////////////////////////////////
int llopen(LinkLayer connectionParameters)
{
    // Open serial port device for reading and writing and not as controlling tty
    // because we don't want to get killed if linenoise sends CTRL-C.
    printf("serialPort: %s\nrole: %d\n", connectionParameters.serialPort, connectionParameters.role);
    int fd = open(connectionParameters.serialPort, O_RDWR | O_NOCTTY);
    printf("FD OPEN: %d\n", fd);
    if (fd < 0)
    {
        perror(connectionParameters.serialPort);
        return -1;
    }

    connectionType = connectionParameters.role;
    tries = connectionParameters.nRetransmissions;
    timeout = connectionParameters.timeout;
    sendReceiveValidate = 0;

    struct termios newtio;
    // Save current port settings
    if (tcgetattr(fd, &oldtio) == -1)
    {
        perror("tcgetattr");
        return -1;
    }
    // Clear struct for new port settings
    memset(&newtio, 0, sizeof(newtio));

    newtio.c_cflag = BAUDRATE | CS8 | CLOCAL | CREAD;
    newtio.c_iflag = IGNPAR;
    newtio.c_oflag = 0;

    // Set input mode (non-canonical, no echo,...)
    newtio.c_lflag = 0;
    newtio.c_cc[VTIME] = 0; // Inter-character timer unused
    newtio.c_cc[VMIN] = 0;  // Blocking read until 5 chars received

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
        return -1;
    }
    printf("New termios structure set\n");

    // Create string to send
    unsigned char buf_set[BUF_SIZE] = {0};
    buf_set[0] = 0x7E;
    buf_set[1] = 0x03;
    buf_set[2] = 0x03;
    buf_set[3] = buf_set[1] ^ buf_set[2];
    buf_set[4] = 0x7E;

    unsigned char buf_s[BUF_SIZE] = {0};
    unsigned char buf_ua[BUF_SIZE] = {0};

    (void)signal(SIGALRM, alarmHandler);

    switch (connectionParameters.role) {
        case LlTx:
            printf("Sent a SET Trama\n");
            setStateMachineTransmitter(fd, buf_set, buf_ua, 0x03, C_BLOCK_UA); // envia set e analisa ua
            break;
 
        case LlRx:
            setStateMachineReceiver(fd, buf_s, 0x03, C_BLOCK_UA); // analisa set e envia ua
            printf("Sent a UA Trama\n");
            break;

        default:
            return -1;
    }

    sleep(1);

    /*
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
        perror("tcsetattr");
        return -1;
    }
    */
   
    return fd;
}

////////////////////////////////////////////////
// LLWRITE
////////////////////////////////////////////////
int llwrite(int fd, const unsigned char *buf, int bufSize) {
    alarmCount = 0;

    if (sendReceiveValidate == 0) {
        frameNumberByte = 0x00;
    }
    else if (sendReceiveValidate == 1) {
        frameNumberByte = 0x40;
    }
    
    //createInformationTrama
    unsigned char buf_inf[bufSize];

    buf_inf[0] = FLAG_BLOCK;
    buf_inf[1] = 0x03;
    buf_inf[2] = frameNumberByte;
    buf_inf[3] = buf_inf[1] ^ buf_inf[2];

    for (int i = 0; i < bufSize; i++) {
        buf_inf[i + 4] = buf[i];
    }

    unsigned char bcc2 = buf[0];

    for (int i = 0; i < bufSize-1; i++) {
        bcc2 ^= buf[i + 1];
    }

    buf_inf[3 + bufSize + 1] = bcc2;
    buf_inf[3 + bufSize + 2] = FLAG_BLOCK;

    fullLength = byte_stuffing(buf_inf, bufSize);
    printf("byte_stuffing size: %d\n", fullLength);

    if (fullLength < 0) {
        sleep(1);

        if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
            perror("tcsetattr");
            return -1;
        }
    }
    printf("fullLength size: %d\n", fullLength);
    int bytesWritten;
    int sentData = FALSE;
    alarmEnabled = FALSE;

    (void)signal(SIGALRM, alarmHandler);
    
    while (!sentData) {
        bytesWritten = sendInfoTrama(fd, buf_inf, fullLength);

        printf("bytesWrittenInfo: %d\n", bytesWritten);

        if (bytesWritten < 0) {
            sleep(1);

            if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
                perror("tcsetattr");
                return -1;
            }
        }

        
        unsigned char wantedBytes[2];
        
        if (frameNumberByte == 0x00) {
            wantedBytes[0] = 0x85;
            wantedBytes[1] = 0x01;
        }

        else if (frameNumberByte == 0x40) {
            wantedBytes[0] = 0x05;
            wantedBytes[1] = 0x81;
        }

        unsigned char buf_super[BUF_SIZE] = {0};

        int resend = setStateMachineReceiverSup(fd, buf_super, 0x03, wantedBytes); // analisa a supervisão
        printf("Resend: %d\n", resend);

        if (resend == 0) {
            sentData = TRUE;
            printf("here\n");
        }

        else if (resend == 1) {
            printf("Received REJ. Sending another Trama\n");
        }
    }

    printf("Received S Trama\n");

    switch (sendReceiveValidate) {
        case 0:
            sendReceiveValidate = 1;
            break;
        case 1: 
            sendReceiveValidate = 0;
            break;
        
        default:
            return -1;
    }
    
    fullLength = bytesWritten - 6; // troquei totalBufferSize por fullLength
    
    return fullLength; // troquei totalBufferSize por fullLength
}

////////////////////////////////////////////////
// LLREAD
////////////////////////////////////////////////
int llread(int fd, unsigned char *packet) {
    int nbytes;
    int bufferIsFull = FALSE;
    int infoSize = 0;

    unsigned char buf_information[(1029 * 2) + 5]; // 1029 * 2 = All bytes that can suffer byte stuffing and therefore duplicate (packet + bcc2). +5 for the inaltered bytes (flag, a, c, bcc1 and flag)
    unsigned char buf_inf[(1029 * 2) + 5];
    
    while (!bufferIsFull) {
        printf("HERE 3\n");
    
        setStateMachineReceiverInf(fd, buf_inf, buf_information, A_BLOCK_INF_TRANS);

        infoSize = sizeof(buf_information);
        printf("buf_information size2: %d\n", infoSize);

        printf("Received I Trama\n");

        nbytes = byte_destuffing(buf_information, infoSize);
        printf("Bytes destuffing: %d\n", nbytes);
        if (nbytes < 0) {
            sleep(1);

            if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
                perror("tcsetattr");
                return -1;
            }
        }

        int receiveSendByte;

        if (buf_information[2] == 0x00) {
            receiveSendByte = 0;
        }
        else if (buf_information[2] == 0x40) {
            receiveSendByte = 1;
        }

        unsigned char bcc2 = buf_information[4];

        for (int i = 0; i < (nbytes-6)-1; i++) {
            bcc2 ^= buf_information[i + 4 + 1];
        }

        if (buf_information[nbytes - 2] == bcc2) { // verificar que bcc2 está correto
            // duplicated trama. Discard information
            if (receiveSendByte != sendReceiveValidate) { // valor de controlo no read for diferente no write, respetivamente
                if (receiveSendByte == 0) {
                    responseByte = 0x85; // RR1
                    sendReceiveValidate = 1;
                }
                else {
                    responseByte = 0x05; // RR0
                    sendReceiveValidate = 0;
                }
            }
            else { // new trama
                for (int i = 0; i < nbytes - 6; i++) {
                    packet[i] = buf_information[4 + i];
                }

                bufferIsFull = TRUE;

                if (receiveSendByte == 0) {
                    responseByte = 0x85;
                    sendReceiveValidate = 1;
                }
                else {
                    responseByte = 0x05;
                    sendReceiveValidate = 0;
                }
            }
        }
        else { // if bcc2 is not correct
            if (receiveSendByte != sendReceiveValidate) { // valor de controlo no read for diferente no write, respetivamente
                if (receiveSendByte == 0) {
                    responseByte = 0x85; // RR1
                    sendReceiveValidate = 1;
                }
                else {
                    responseByte = 0x05; // RR0
                    sendReceiveValidate = 0;
                }
            }
            else { // new trama
                if (receiveSendByte == 0) {
                    responseByte = 0x01; // REJ0
                    sendReceiveValidate = 0;
                }
                else {
                    responseByte = 0x81; // REJ1
                    sendReceiveValidate = 1;
                }
            }
        }

        // create S trama
        unsigned char buf_super[BUF_SIZE + 1];

        buf_super[0] = FLAG_BLOCK;
        buf_super[1] = 0x03; 
        buf_super[2] = responseByte;
        buf_super[3] = buf_super[1] ^ buf_super[2];
        buf_super[4] = FLAG_BLOCK;
        buf_super[5] = '\n';
        
        int bytesWrite = write(fd, buf_super, BUF_SIZE);

        if (bytesWrite < 0) {
            sleep(1);

            if (tcsetattr(fd, TCSANOW, &oldtio) == -1) {
                perror("tcsetattr");
                return -1;
            }
        }

        printf("Supervision trama has been sent");
    }

    fullLength = nbytes - 6;

    return fullLength; 
}

////////////////////////////////////////////////
// LLCLOSE
////////////////////////////////////////////////
int llclose(int fd)
{
    unsigned char buf_disc_trans[BUF_SIZE + 1] = {0};

    unsigned char buf_trans[BUF_SIZE + 1] = {0};
    buf_trans[0] = FLAG_BLOCK;
    buf_trans[1] = A_BLOCK_DISC_TRANS;
    buf_trans[2] = C_BLOCK_DISC;
    buf_trans[3] = (A_BLOCK_DISC_TRANS ^ C_BLOCK_DISC);
    buf_trans[4] = FLAG_BLOCK;
    buf_trans[5] = '\n';
   
    unsigned char buf_rec[BUF_SIZE + 1] = {0};
    unsigned char buf_ua[BUF_SIZE + 1] = {FLAG_BLOCK, A_BLOCK_UA, C_BLOCK_UA, BCC_BLOCK_UA, FLAG_BLOCK, '\n'};

    (void)signal(SIGALRM, alarmHandler);

    int count = 0;
    while (count <= 3) { // mudou para 3
        if (connectionType == TRANSMITTER) {
            if (count > 0) { // mudei para > 0
                write(fd, buf_ua, 5);
                printf("Sent a UA trama");
                count++;
            } 
            else {
                printf("Sent a Disconnect trama");
                setStateMachineTransmitter(fd, buf_trans, buf_disc_trans, 0x01, C_BLOCK_DISC); // 0x03 pq é reply do receiver
                count++;

                if (buf_trans[2] != C_BLOCK_DISC) {
                    printf("Not a Disconnect trama");
                    count = 0;
                }
            }
        }

        else if (connectionType == RECEIVER) {
            if (count > 0) { // mudei para > 0
                setStateMachineReceiverDisc(fd, buf_rec, A_BLOCK_UA, C_BLOCK_UA);
                printf("Received UA trama");
                count++;
                break;
            }
            else {
                setStateMachineReceiverDisc(fd, buf_rec, A_BLOCK_DISC_TRANS, C_BLOCK_DISC);
                count++;
                buf_rec[0] = FLAG_BLOCK;
                buf_rec[1] = 0x01;
                buf_rec[2] = C_BLOCK_DISC;
                buf_rec[3] = buf_rec[1] ^ buf_rec[2];
                buf_rec[4] = FLAG_BLOCK;
                buf_rec[5] = '\n';

                write(fd, buf_rec, BUF_SIZE);

                printf("Received a Disconnect trama");

                if (buf_rec[2] != C_BLOCK_DISC) {
                    printf("Not a Disconnect trama");
                    count = 0;
                }
            }
        }
        else count = 0;
    }

    sleep(1);
    
    // Restore the old port settings
    if (tcsetattr(fd, TCSANOW, &oldtio) == -1)
    {
        perror("tcsetattr");
        return -1;
    }

    if (close(fd) < 0) {
        perror("close");
        return -1;
    }

    return fd;
}

int byte_stuffing(unsigned char* new_message, int bufSize) {
    unsigned char tmp[bufSize + 6];

    for (int i = 0; i < bufSize + 6 ; i++) {
        tmp[i] = new_message[i];
    }

    int newLength = 4;

    for (int i = 4; i < (bufSize + 6); i++) {
        if (tmp[i] == FLAG_BLOCK && i != (bufSize + 5)) {
            new_message[newLength] = 0x7D;
            new_message[newLength+1] = 0x5E;
            newLength = newLength + 2;
        }

        else if (tmp[i] == 0x7D && i != (bufSize + 5)) {
            new_message[newLength] = 0x7D;
            new_message[newLength+1] = 0x5D;
            newLength = newLength + 2;
        }

        else {
            new_message[newLength] = tmp[i];
            newLength++;
        }
    }

    return newLength;
}

int byte_destuffing(unsigned char* new_message, int bufSize) {
    unsigned char tmp[bufSize + 5];

    for (int i = 0; i < bufSize + 5; i++) {
        tmp[i] = new_message[i];
    }

    int newLength = 4;

    for (int i = 4; i < bufSize + 5; i++) {
        if (tmp[i] == 0x7D) {
            if (tmp[i + 1] == 0x5D) {
                new_message[newLength] = 0x7D;
            }
            else if (tmp[i + 1] == 0x5E) {
                new_message[newLength] = FLAG_BLOCK;
            }
            i++;
            newLength++;
        }

        else {
            new_message[newLength] = tmp[i];
            newLength++;
        }
    }

    return newLength;
}