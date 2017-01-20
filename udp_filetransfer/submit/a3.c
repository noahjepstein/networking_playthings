/*
    Filename: a3.c
    Author: Noah J. Epstein
    Date Created: 10.16.2016

    This file implements a the go-back-n file transfer protocol using UDP.

    Compile:  > gcc -g a3.c -lnsl

    Run:      > ./a.out PORTNUM [-verbose]

    Contact: noahjepstein@gmail.com
*/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <netdb.h>
#include <sys/select.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/fcntl.h>
#include <sys/stat.h>
#include <netinet/in.h>
#include <arpa/inet.h>

/****************************** GLOBAL CONSTS *********************************/

#define DEBUG_MODE 0

#if DEBUG_MODE
  #define DEBUG_PRINT(s) printf s
#else
  #define DEBUG_PRINT(s) (void) 0
#endif

#define WRITEBUFSIZE 1024
#define MAX_QUEUESIZE 50
#define FILENAMESIZE 20
#define READBUFLEN 512
#define MAX_DATASIZE 512
#define RRQSIZE 22
#define MAX_MSGSIZE 514
#define TYPESIZE 1
#define WINDSIZE 1
#define ACKSIZE 2
#define SEQSIZE 1
#define RRQ 1
#define DATA 2
#define ACK 3
#define ERROR 4
#define TIMEOUT_SEC 3.0
#define MAX_TIMEOUTS 5


/***************************** TYPE DEFINITONS ********************************/

typedef struct Message {
    char type;          /* RRQ = 1; DATA = 2; ACK = 3; ERROR = 4 */
    char cwnd;          /* valid values are 1-9 */
    char seqnum;        /* valid values 0-50 -> max file size is 512 * 50 */
    char filename[FILENAMESIZE];  /* filename at most 19 chars + null */
    char data[WRITEBUFSIZE];     /* can contain no data */
    int datalen;
    bool acked;
    bool sent;
    int nTimeouts;
} Message;

typedef struct MessageQueue {
    int nEnqueued;
    int window;
    Message msgs[MAX_QUEUESIZE];
} MessageQueue;

typedef enum {LOW, HIGH} Verbosity;

/***************************** FUNC PROTOTYPES ********************************/

void error(const char *msg);

// void DEBUG_PRINT(const char* msg);

bool processRequest(char* requestbuf);

bool makeMsgQueue(char* filename);

bool makeErrorMsg();

void sendMsgQueue(int sockfd, struct sockaddr_in* cli_addr, socklen_t* clilen);

void writeToBuffer(int i);

bool timeout(clock_t t1, clock_t t2);



/******************************* GLOBAL VARS **********************************/

Verbosity verb;
char filename[FILENAMESIZE];
char readbuf[READBUFLEN];
char writebuf[WRITEBUFSIZE];
MessageQueue msgQ;

/******************************************************************************/


int main(int argc, char* argv[]) {

    /*** var/obj initialization ***/

    int sockfd, newsockfd, portno, n, select_n, optval, flags;
    struct sockaddr_in serv_addr, cli_addr, *pV4Addr;
    struct hostent *hostp;
    struct timeval tv;
    char* hostaddrp;
    bool msgComplete;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    socklen_t clilen = sizeof(cli_addr);

    /* input processing */

    if ((argc < 2) || (argc > 3)) {
        fprintf(stderr,"ERROR, proper usage is >./a.out PORTNUM [-verbose] \n");
        exit(1);
    }
    if (argc == 3) {
        if ((strcmp(argv[2], "-v") == 0) || (strcmp(argv[2], "-verbose") == 0)){
            verb = HIGH;
        } else {
            fprintf(stderr,
                    "ERROR, proper usage is >./a.out PORTNUM [-verbose] \n");
            exit(1);
        }
    } else {
        printf("Note: to turn on debug-mode printout statements, set define "
               "statement \"#define DEBUG_MODE\" to 1. \n");
    }

    /* main sock initialization */

    portno = atoi(argv[1]);
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
       error("ERROR creating socket");

    /* use this option so that the os doesn't hold onto the port after server
       shutdown (for debugging, running server multiple times in succession) */

    optval = 1;
    setsockopt(sockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
                                                                   sizeof(int));


    bzero((char*) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
         error("ERROR on binding");

    /*** MAIN PROCESSING LOOP */

    while (1) {

        /* recieve initial message from a client */

        /* clear readbuffer */
        memset(readbuf, 0, READBUFLEN);
        memset((char*) msgQ.msgs, 0, MAX_QUEUESIZE * sizeof(Message));
        msgQ.nEnqueued = 0;

        tv.tv_sec = 180;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));

        n = recvfrom(sockfd, readbuf, READBUFLEN, 0,
                                        (struct sockaddr *) &cli_addr, &clilen);

        if (n < 0) {
            DEBUG_PRINT(("recvfrom error: timeout on inital rrq recieve\n"));
            continue;

        } else if (n == ACKSIZE) {
            DEBUG_PRINT(("Fault: Getting ack on initial read from client. Ignore.\n"));
            continue;

        } else if ((n > ACKSIZE) && (n <= RRQSIZE)) {
            DEBUG_PRINT(("Got RRQ. Processing and making response.\n"));
            msgComplete = processRequest(readbuf);

        } else {
            DEBUG_PRINT(("Malformed message to server. Ignoring message. \n"));
            DEBUG_PRINT(("Message appeared to contain %d bytes.\n", n));
            DEBUG_PRINT(("Message text is as follows: \n%s\n", readbuf));
            continue;
        }

        if (msgComplete) {
            sendMsgQueue(sockfd, &cli_addr, &clilen);
        } else {
            DEBUG_PRINT(("Message constructor failed.\n"));
        }
    }

    DEBUG_PRINT(("Terminating server.\n"));
    close(sockfd);
    return 0;
}

/********************************* FUNC DFNS **********************************/

/* exits with code 1 and prints the error message 'msg' */
void error(const char *msg) {
    perror(msg);
    exit(1);
}

/* processes a rrq request for a file, and enqueues the sequence of messages to
 * be sent. */
bool processRequest(char* requestbuf) {

    bool success = false;
    int type = (int) requestbuf[0];
    int cwnd = (int) requestbuf[1];
    strncpy(filename, &requestbuf[2], FILENAMESIZE);

    if (type != RRQ) {

        DEBUG_PRINT(("Packet of size RRQSIZE: 22 did not have type RRQ\n"));
        return false;

    } else {

        DEBUG_PRINT(("Recieved request for file \"%s\"\n", filename));

        if( access(filename, F_OK) != -1 ) {
            msgQ.window = cwnd;

            DEBUG_PRINT(("Found %s. Making into message.\n", filename));
            success = makeMsgQueue(filename);

        } else {
            DEBUG_PRINT(("File \"%s\" not found. Enqueuing error message.\n",
                                                                     filename));
            success = makeErrorMsg();
        }
    }

    return success;
}

/* makes queue of messages */
bool makeMsgQueue(char* filename) {

    int fd;
    int n = MAX_DATASIZE;
    int numpacks;
    int i = 0;
    bool success = false;
    struct stat st;
    long offset = 0;

    FILE *fp = fopen(filename, "r");
    if (fp == NULL) {
        error("Error opening file.");
    } else {
        fd = fileno(fp);
        stat(filename, &st);
        numpacks = st.st_size / MAX_DATASIZE + 1;
        DEBUG_PRINT(("File size: %d bytes. \n", st.st_size));
        DEBUG_PRINT(("Will require %d packet(s).\n", numpacks));
    }


    while ((n == MAX_DATASIZE) && (i < MAX_QUEUESIZE)) {
        n = pread(fd, msgQ.msgs[i].data, MAX_DATASIZE, (off_t) offset);
        offset = offset + n;
        DEBUG_PRINT(("Creating packet number %d of %d with "
                     "%d bytes of data.\n", i + 1, numpacks, n));
        msgQ.msgs[i].seqnum = (char)i;
        msgQ.msgs[i].type = DATA;
        msgQ.msgs[i].acked = false;
        msgQ.msgs[i].datalen = n;
        msgQ.nEnqueued++;
        i++;
    }

    if (n >= 0) {
        success = true;
    } else if (n == -1) {
        error("Error reading from file.");
    }

    fclose(fp);
    return success;
}


/* makes an error message and adds it to the message queue */
bool makeErrorMsg() {
    msgQ.msgs[0].type = (char) ERROR;
    msgQ.msgs[0].acked = false;
    msgQ.msgs[0].datalen = 1;
    msgQ.msgs[0].nTimeouts = 0;
    msgQ.nEnqueued = 1;
    msgQ.window = (char) 1;
    return true;
}

/* sends the queue of 512-byte data chunks to the client. */
/* does timeout and ack checking */
void sendMsgQueue(int sockfd, struct sockaddr_in* cli_addr, socklen_t* clilen) {

    DEBUG_PRINT(("Sending message queue.\n"));
    int i, n, windowBottom, windowTop;
    int greatestAck = -1;
    int wblen = 0;
    bool tout;
    struct timeval tv;

    /* send the whole stack */
    while ((greatestAck + 1) < msgQ.nEnqueued) {

        windowBottom = greatestAck + 1;
        windowTop = windowBottom + msgQ.window;

        /* send all the packets in the window,
         * BUT ONLY IF: the message has timed out or you haven't sent it before
         */
        tout = false;

        for (i = greatestAck + 1;
                (i < windowTop) &&
                (i < MAX_QUEUESIZE) &&
                (i < msgQ.nEnqueued); i++) {

            if (msgQ.msgs[i].nTimeouts >= MAX_TIMEOUTS) {
                DEBUG_PRINT(("MAX NUMBER OF TIMEOUTS EXCEEDED 5!! "
                             "SCREW THE CLIENT! EJECT!\n"));
                return;
            }

            if (tout || (!msgQ.msgs[i].sent)) {

                writeToBuffer(i); /* put an outgoing message in the buffer */

                if (msgQ.msgs[i].type == ERROR) {
                    DEBUG_PRINT(("Sending error message.\n"));
                    sendto(sockfd, writebuf, TYPESIZE, 0,
                                         (struct sockaddr *) cli_addr, *clilen);
                    return;
                }

                sendto(sockfd, writebuf, msgQ.msgs[i].datalen + ACKSIZE, 0,
                                         (struct sockaddr *) cli_addr, *clilen);
                msgQ.msgs[i].sent = true;
                DEBUG_PRINT(("Sent packet %d of %d.\n",
                                                      (i + 1), msgQ.nEnqueued));
            }
        }

        /* read in a packet. see if it's an ack for the lowest message in
         * the window. */

        tv.tv_sec = 3;
        tv.tv_usec = 0;
        setsockopt(sockfd, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof(tv));
        bzero(readbuf, READBUFLEN);

        n = recvfrom(sockfd, readbuf, READBUFLEN, 0,
                                          (struct sockaddr *) cli_addr, clilen);

        if (n < 0) {
            int err = errno;
            DEBUG_PRINT(("Timeout occured when waiting on ack with seqnum: %d\n",
                         greatestAck + 1));
            msgQ.msgs[greatestAck + 1].nTimeouts++;
            tout = true;

        }

        if ((n != ACKSIZE) || (readbuf[0] != ACK)) {
            continue;

        } else {

            if (readbuf[1] == (char)(greatestAck + 1)) {
                DEBUG_PRINT(("Acknowledged reciept of message %d.\n",
                                                                   readbuf[1]));
                greatestAck++;
            } else {
                DEBUG_PRINT(("Recieved ack number %d when looking for ack with "
                             "seqnum: %d.\n", readbuf[1], greatestAck + 1));
                tout = true;
            }
        }
    }
    DEBUG_PRINT(("Done sending message queue of %d packets.\n", msgQ.nEnqueued));
}

/* writes to the write buffer to prepare a message for sending */
void writeToBuffer(int i) {

    bzero(writebuf, WRITEBUFSIZE);
    writebuf[0] = msgQ.msgs[i].type;
    if (writebuf[0] == ERROR) {
        return;
    }
    writebuf[1] = msgQ.msgs[i].seqnum;
    memcpy(writebuf + 2, msgQ.msgs[i].data, msgQ.msgs[i].datalen);
}
