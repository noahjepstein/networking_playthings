/*
 * udpclient.c - A simple UDP client
 * usage: udpclient <host> <port>
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netdb.h>

#define BUFSIZE 512
#define FILESIZE 20
#define WRITEBUFSIZE 1024
#define MAX_QUEUESIZE 50
#define FILENAMESIZE 20
#define READBUFLEN 512
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

typedef struct __attribute__((__packed__)) Message {
    char type;          /* RRQ = 1; DATA = 2; ACK = 3; ERROR = 4 */
    char cwnd;          /* valid values are 1-9 */
    char seqnum;        /* valid values 0-50 -> max file size is 512 * 50 */
    char filename[FILESIZE];  /* filename at most 19 chars + null */
    char data[BUFSIZE];     /* can contain no data */
} Message;

/*
 * error - wrapper for perror
 */
void error(char *msg) {
    perror(msg);
    exit(0);
}

int main(int argc, char **argv) {
    int sockfd, portno, n;
    int serverlen;
    struct sockaddr_in serveraddr;
    struct hostent *server;
    char *hostname;
    char buf[BUFSIZE];

    /* check command line arguments */
    if (argc != 3) {
       fprintf(stderr,"usage: %s <hostname> <port>\n", argv[0]);
       exit(0);
    }
    hostname = argv[1];
    portno = atoi(argv[2]);

    /* socket: create the socket */
    sockfd = socket(AF_INET, SOCK_DGRAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    /* gethostbyname: get the server's DNS entry */
    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host as %s\n", hostname);
        exit(0);
    }

    /* build the server's Internet address */
    bzero((char *) &serveraddr, sizeof(serveraddr));
    serveraddr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
	  (char *)&serveraddr.sin_addr.s_addr, server->h_length);
    serveraddr.sin_port = htons(portno);
    serverlen = sizeof(serveraddr);

    /* get a message from the user */
    bzero(buf, BUFSIZE);

    /* rrq formation */
    char rrq[RRQSIZE];
    bzero(rrq, RRQSIZE);
    rrq[0] = 1;
    rrq[1] = 1; // window size
    char* filename = "t6.data";
    strncpy(rrq + 2, filename, strlen(filename) + 1);
    printf("sending rrq for file: %s\n", rrq + 2);

    n = sendto(sockfd, rrq, RRQSIZE, 0, (struct sockaddr *) &serveraddr,
                                                                     serverlen);
    if (n < 0)
      error("ERROR in sendto");

    /* print the server's reply */
    bool done = false;
    char ack[2];

    char* data = malloc(MAX_MSGSIZE * 50);
    int total_bytes = 0;
    int num_packets = 0;

    while (!done) {

        bzero(buf, BUFSIZE);
        n = recvfrom(sockfd, buf, MAX_MSGSIZE + 1, 0,
                 (struct sockaddr *) &serveraddr, (unsigned int *) &serverlen);
        memcpy(data + total_bytes, buf, n);
        total_bytes = total_bytes + n;
        num_packets++;

        ack[0] = ACK; /* make the packet type ACK */
        ack[1] = buf[1]; /* give the packet a sequence number */
        printf("num bytes recieved in this packet: %d\n", n);
        printf("total num bytes recieved: %d\n", total_bytes);
        printf("total num packets recieved: %d\n", num_packets);
        printf("packet sequence number: %d\n", ack[1]);
        data[total_bytes + 1] = (char) 0;

        /* add 2 to data pointer to skip the prefix which may include null
         * terminator and will screw up the printout */
         printf("packet data recieved:\n%s\n", buf + 2);
        printf("data recieved to date:\n%s\n", data + 2);

        if (n < 0) {
            error("ERROR in recvfrom");

        } else if (n < MAX_MSGSIZE) {
            printf("packet %d is the last packet.\n", ack[1]);
            done = true;
        }

        printf("Sending ack: %d%d\n", ack[0], ack[1]);
        if (buf[1] != 5)
            sendto(sockfd, ack, ACKSIZE, 0, (struct sockaddr *) &serveraddr,
                                                                    serverlen);
    }
    free(data);
    return 0;
}
