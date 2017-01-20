/*
    Filename: a2.c
    Author: Noah J. Epstein
    Date Modified: 10.10.2016
    Date Created: 9.30.2016


    This file satisfies assignment 2 for F16 -- Comp112.

    This file implements a chat application based on the client-server model.

    Compile:  > gcc -g a2.c -lnsl

    Usage:  To run server:
              > ./a.out 9000      # port number (ex 9000) is the only argument

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
#include <netinet/in.h>
#include <arpa/inet.h>

/****************************** GLOBAL CONSTS *********************************/

/* NORMAL MSG TYPES */
#define HELLO 1
#define HELLO_ACK 2
#define LIST_REQUEST 3
#define CLIENT_LIST 4
#define CHAT 5
#define EXIT 6
/* ERR MSG TYPES */
#define ERR_CLIENT_ALREADY_PRESENT 7
#define ERR_CANNOT_DELIVER 8

/* SIZES */
#define DATA_MAX_SIZE 400
#define MAX_NCLIENTS 20
#define PACK_SIZE 451 // max size of message is 400 data + 50 header
#define ID_LEN 20 //size of client identifier
#define HDR_SIZE 50 // header is 50 bytes

/* NAMES */
#define SERVER_NAME "Server"
#define EMPTY_FD -100

#define TVSEC 0
#define TVUSEC 500000

/***************************** TYPE DEFINITONS ********************************/

// 50 bytes packed
typedef struct __attribute__((__packed__)) Header {
    unsigned short type;
    char src[ID_LEN];
    char dst[ID_LEN];
    unsigned int len;
    unsigned int msgID;
} Header;

typedef struct __attribute__((__packed__)) Message {
    Header hdr; // 50 bytes max
    char* data; // 400 bytes max
} Message;

typedef struct MessageList {
    Message msg;
    struct MessageList* tl;
} *MessageList ;

typedef char ClientID[ID_LEN];

typedef struct Client {
    ClientID id;
    int fd; // their socket file descriptor
    MessageList msgToSend; // data that we need to send to client
    Message* partial;
    int pbytes;
    bool hasPartial;
} Client;

typedef struct CliListBare {
    char* data;
    unsigned int len;
} CliListBare;

typedef Client ClientArr[MAX_NCLIENTS];

typedef enum {LOW, HIGH} Verbosity;
typedef enum {READ, WRITE} SMode;


/***************************** FUNC PROTOTYPES ********************************/

#define MIN(a, b) (((a) < (b)) ? (a) : (b))

void revEndian(char* input, unsigned int nBytes);

#define REVEND(bytes) revEndian(bytes, sizeof(bytes))

void error(const char *msg);

void printMsg(const char* msg);

int selectWrapper(fd_set* setptr, SMode s);

void remClient(int clientfd, ClientArr clients);

void readMsg(int src_fd);

void processHello(int src_fd, Header* pheader);

bool clientExistsID(ClientID cid);

bool clientExistsFD(int cli_sock_fd);

void makeMsg(int recvr, unsigned short msg_type, Header* pheader, char* data);

void newClient(int newcli_fd, Header* pheader, ClientArr clients);

void initCliLists();

int indOfCliFD(int cli_fd, ClientArr clients);

int indOfCliID(int cli_id, ClientArr clients);

CliListBare makeClientList();

MessageList getTail(MessageList ml);

int ID2FD(ClientID clid, ClientArr clients);

void writeMsgs(int dst_fd);

/****************************** GLOBAL  *******************************/

Verbosity verb = LOW;
struct timeval TV;
int sockmax = 0;
fd_set activefds, queryfds;
ClientArr cli_arr;
ClientArr temps_arr; // only clis who failed their first-time connection
CliListBare clb;
Client EMPTY_CLIENT;
char hdr[HDR_SIZE],
     data[DATA_MAX_SIZE],
     packet[PACK_SIZE],
     clbuf[DATA_MAX_SIZE];

/******************************************************************************/


int main(int argc, char* argv[]) {

    /*** var/obj initialization ***/

    int mainsockfd, newsockfd, portno, n, select_n, optval, i, j, flags;
    char cli_ip_str[INET_ADDRSTRLEN];
    struct sockaddr_in serv_addr, cli_addr, *pV4Addr;
    struct in_addr ipAddr;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    socklen_t clilen = sizeof(cli_addr);

    TV.tv_sec = TVSEC;
    TV.tv_usec = TVUSEC;
    FD_ZERO(&queryfds);
    FD_ZERO(&activefds);

    EMPTY_CLIENT.id[0] = '\0';
    EMPTY_CLIENT.fd = EMPTY_FD;
    initCliLists();



    /* input processing */

    if ((argc < 2) || (argc > 3)) {
        fprintf(stderr,"ERROR, proper usage is >./a.out PORTNUM [-verbosity] \n");
        exit(1);
    }

    portno = atoi(argv[1]);

    if (argc == 3) {
        if (strcmp(argv[2], "-v") == 0) {
            verb = HIGH;
        } else {
            fprintf(stderr,"ERROR, proper usage is >./a.out PORTNUM [-verbosity] \n");
            exit(1);
        }
    }

    /* main sock initialization */

    mainsockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (mainsockfd < 0)
       error("ERROR opening socket");

    /* make the main (connection accepting) socket nonblocking */
    // flags = fcntl(mainsockfd, F_GETFL, 0);
    // fcntl(mainsockfd, F_SETFL, O_NONBLOCK);
    // fcntl(mainsockfd, F_SETFL, flags | O_NONBLOCK);

    /* use this option so that the os doesn't hold onto the port after server
       shutdown (for debugging, running server multiple times in succession) */
    optval = 1;
    setsockopt(mainsockfd, SOL_SOCKET, SO_REUSEADDR, (const void *)&optval,
                                                                   sizeof(int));

    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = htonl(INADDR_ANY);
    serv_addr.sin_port = htons(portno);

    if (bind(mainsockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
         error("ERROR on binding");

    listen(mainsockfd, 5);
    FD_SET(mainsockfd, &activefds); /* add socket fd */
    sockmax = mainsockfd;

    /*** MAIN PROCESSING LOOP */

    while (1) {

        j = 0;

        /* select for reading and error check */
        FD_ZERO(&queryfds);
        queryfds = activefds;
        select_n = selectWrapper(&queryfds, READ);

        /* go through all the fds that are eligible for reading.
           accept connections on our main socket
           otherwise just read into the buffer. */
        for (i = 0; ((i < FD_SETSIZE) && (j < select_n)); i++) {

            /* accept new connections, but repeatedly checking if mainsockfd is
               set may do some weird shit? */
            if ((i == mainsockfd) && (FD_ISSET(mainsockfd, &queryfds))) {

                printMsg("ACCEPTING connection!");
                newsockfd = accept(mainsockfd, (struct sockaddr *) &cli_addr,
                                                                       &clilen);
                if (newsockfd < 0)
                    error("ERROR on accept");
                if (newsockfd > sockmax) sockmax = newsockfd;

                fcntl(newsockfd, F_SETFL, fcntl(newsockfd,
                                                      F_GETFL, 0) | O_NONBLOCK);

                // don't want to read from these in current call...how?
                FD_SET(newsockfd, &activefds);
                j++;

            } else if (FD_ISSET(i, &queryfds)) {

                readMsg(i);
                j++;
            }
        }

        /* now write to any sockets that are ready for it and
           need to be written to */
        FD_ZERO(&queryfds);
        j = 0;
        queryfds = activefds;
        FD_CLR(mainsockfd, &queryfds);
        select_n = selectWrapper(&queryfds, WRITE);

        for (i = 0; ((i < FD_SETSIZE) && (j < select_n)); i++) {
            if (FD_ISSET(i, &queryfds)) {
                writeMsgs(i);
                j++;
            }
        }
        FD_ZERO(&queryfds);
    }

    printMsg("terminating server");
    close(mainsockfd);
    return 0;
}

/***************************** FUNC DFNS ********************************/

/* exits with code 1 and prints the error message 'msg' */
void error(const char *msg) {
    perror(msg);
    exit(1);
}

/* prints messages to stdout when verbosity is turned on */
void printMsg(const char* msg) {
    if (verb == LOW) {
        return;
    } else {
        printf("%s\n", msg);
    }
}

/* does some error checking after calling select */
int selectWrapper(fd_set* setptr, SMode mode) {

    int n;
    if (mode == READ) {
        n = select(sockmax + 1, setptr, NULL, NULL, &TV);
        // printf("%d file descriptor(s) ready for reading\n", n);

    } else if (mode == WRITE) {
        n = select(sockmax + 1, NULL, setptr, NULL, &TV);
        // printf("%d file descriptor(s) ready for writing\n", n);

    } else
        error("incorrect argument to selectWrapper");

    if (n < 0) {
        printMsg("select error");
        error("select error");
    } else if (n == 0) {
        // printMsg("select timed out");
        TV.tv_sec = TVSEC;
        TV.tv_usec = TVUSEC;
    }
    return n;
}

/* note: only removes clients who exist already; otherwise socket just closed */
void remClient(int clientfd, ClientArr clients) {

    int i;
    MessageList freedom;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if (clients[i].fd == clientfd) {
            while (clients[i].msgToSend != NULL) {
                freedom = clients[i].msgToSend;
                clients[i].msgToSend = clients[i].msgToSend->tl;
                free(freedom);
            }
            clients[i] = EMPTY_CLIENT;
        }
    }

    FD_CLR(clientfd, &activefds);
    FD_CLR(clientfd, &queryfds);
    close(clientfd);
}

/* readMsg: input message reading from buffers
   since the only valid messages one can send to server are types
   HELLO, LIST REQUEST, CHAT, and EXIT, we determine if the message is one of
   those four and process the following header; if it isn't one of those four,
   we simply disconnect the offending client.
   error checking for input message construction */
void readMsg(int src_fd) {

    bzero(hdr, HDR_SIZE);
    int n = read(src_fd, hdr, HDR_SIZE);

    if (n < 0) {

        printMsg("client socket unexpectedly closed");
        remClient(src_fd, cli_arr);

    } else if (n == 0) {
        printMsg("client closed socket");
        remClient(src_fd, cli_arr);

    } else if (n < HDR_SIZE) {
        printMsg("client did not send enough bytes");
        // TODO: figure out partial recieve architecture
        // TODO: if (!clientExistsFD(src_fd, cli_arr))
        // TODO: append to partial until we get 50 bytes. then check.
        remClient(src_fd, cli_arr);

    } else { // read the header

        Header* hptr = (Header *) &hdr;
        hptr->type = ntohs(hptr->type);
        hptr->len = ntohl(hptr->len);
        hptr->msgID = ntohl(hptr->msgID);
        if (strcmp(hptr->src, "Server") == 0) {
            printMsg("Client used illegal indentifier \"Server\"");
        }

        switch(hptr->type) {

            case HELLO:
                processHello(src_fd, hptr);
                break;

            case LIST_REQUEST:

                printMsg("recieved LIST_REQUEST");

                if ( (!clientExistsFD(src_fd)) ||
                     (strcmp(hptr->src, cli_arr[indOfCliFD(src_fd, cli_arr)].id) != 0)) {
                    printMsg("unknown client sending list request");
                    remClient(src_fd, cli_arr);
                    break;
                }
                printMsg("sending list to client");
                makeMsg(src_fd, LIST_REQUEST, hptr, NULL);
                break;

            case CHAT:

                printMsg("recieved CHAT");

                // check if sender has said hello yet. if not, remove sender.
                if ((!clientExistsFD(src_fd)) ||
                    (!clientExistsID(hptr->src))) {
                    printMsg("unknown client sending chat message");
                    remClient(src_fd, cli_arr);
                    break;
                }  else if (strcmp(hptr->src,
                                cli_arr[indOfCliFD(src_fd, cli_arr)].id) != 0) {
                    printMsg("client changed ClientID during connection");
                    remClient(src_fd, cli_arr);
                }

                // check if recipient is a valid client. if not, cannot deliver error.
                // printf("dest for chat: %s\n", hptr->dst);
                if (!clientExistsID(hptr->dst)) {
                    printMsg("Client does not exist -- cannot deliver message.");
                    makeMsg(src_fd, ERR_CANNOT_DELIVER, hptr, NULL);
                    break;
                }

                // check that client is not trying to send to self
                if (strcmp(hptr->src, hptr->dst) == 0) {
                    printMsg("client trying to send message to themselves");
                    remClient(src_fd, cli_arr);
                    break;
                }

                /* doesn't work with provided client
                // if (hptr->msgID < 1) {
                //     printMsg("Invalid message format: msgID < 1. ");
                //     remClient(src_fd, cli_arr);
                //     break;
                // } */

                bzero(data, DATA_MAX_SIZE);
                n = read(src_fd, data, hptr->len);
                // printf("n: %d hptr->len %d\n", n, hptr->len);

                if (n != hptr->len) {

                    // incapable of handling partial data
                    printMsg("client message malformed, incorrect length");
                    remClient(src_fd, cli_arr);
                    break;

                } else {

                    printMsg("making ze good chat message");
                    makeMsg(ID2FD(hptr->dst, cli_arr), CHAT, hptr, data);
                }

                break;

            case EXIT:

                printMsg("recieved EXIT request -- removing client");
                remClient(src_fd, cli_arr);
                break;

            default:

                printMsg("client sent invalid request type to server");
                remClient(src_fd, cli_arr);
        }
    }
}

void processHello(int src_fd, Header* pheader) {

    if (clientExistsFD(src_fd)) {
        printMsg("connected client sent repeat hello message. to be disconnected");
        newClient(src_fd, pheader, temps_arr);
        makeMsg(src_fd, ERR_CLIENT_ALREADY_PRESENT, pheader, NULL);

    } else if (clientExistsID(pheader->src)) {
        printMsg("new client has same name as exisiting client. disconnecting new client.");
        remClient(src_fd, cli_arr);

    } else if ((pheader->len != 0) || (pheader->msgID != 0) ||
               (strcmp(pheader->src, "Server")) == 0) {
        printMsg("Invalid message format.");
        remClient(src_fd, cli_arr);

    } else {
        printMsg("making new client and acknowledging");
        newClient(src_fd, pheader, cli_arr);
        makeMsg(src_fd, HELLO_ACK, pheader, NULL);
    }
}

// checks if client exists by looking up their ID (username)
bool clientExistsID(ClientID cid) {

    int i;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if (strcmp(cli_arr[i].id, cid) == 0) {
            return true;
        }
    }

    return false;
}

/* checks if client exists by looking to see if they have an open socket fd */
bool clientExistsFD(int cli_sock_fd) {

    int i;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if (cli_arr[i].fd == cli_sock_fd) {
            return true;
        }
    }
    return false;
}

// invariants
// recvr fd is always associated wtih pheader->src
void makeMsg(int recvr, unsigned short msg_type, Header* pheader, char* data) {

    assert(pheader != NULL);
    Message msg1;
    Message msg2;
    MessageList ml = malloc(sizeof(struct MessageList));
    assert(ml);
    char* clstr;
    printMsg("makeMsg called");

    switch (msg_type) {

        case HELLO_ACK:

            printMsg("making HELLO_ACK message");
            if (!clientExistsFD(recvr))
                error("client (ack recipient) does not exist");
            msg1.hdr.type = HELLO_ACK;
            strcpy(msg1.hdr.src, SERVER_NAME);
            strcpy(msg1.hdr.dst, pheader->src);
            msg1.hdr.len = 0;
            msg1.hdr.msgID = 0;
            msg1.data = NULL;
            ml->msg = msg1;
            ml->tl = NULL;
            cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend = ml;

        case CLIENT_LIST:

            printMsg("making CLIENT_LIST message!");
            if (!clientExistsFD(recvr))
                error("client (recipient of list) does not exist");
            msg2.hdr.type = CLIENT_LIST;
            strcpy(msg2.hdr.src, SERVER_NAME);
            strcpy(msg2.hdr.dst, pheader->src);
            clb = makeClientList();
            msg2.hdr.len = clb.len;
            msg2.hdr.msgID = 0;
            msg2.data = clb.data;
            ml = malloc(sizeof(struct MessageList));
            assert(ml);
            ml->msg = msg2;
            ml->tl = NULL;

            if (cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend == NULL) {
                cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend = ml;
            } else {
                MessageList last =
                         getTail(cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend);
                last->tl = ml;
            }
            break;

        case CHAT:

            printMsg("Making CHAT message.");

            if (!clientExistsID(pheader->src))
                error("client (sender) does not exist");
            if (!clientExistsID(pheader->dst) || !clientExistsFD(recvr))
                error("calling chat message creation on nonexisting client");

            msg1.hdr.type = CHAT;
            strcpy(msg1.hdr.src, pheader->src);
            strcpy(msg1.hdr.dst, pheader->dst);
            msg1.hdr.msgID = pheader->msgID;
            msg1.hdr.len = pheader->len;
            msg1.data = malloc(msg1.hdr.len);
            assert(msg1.data);
            memcpy(msg1.data, data, msg1.hdr.len);
            ml->msg = msg1;
            ml->tl = NULL;
            if (cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend == NULL) {
                cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend = ml;
            } else {
                MessageList last = getTail(cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend);
                last->tl = ml;
            }
            break;

        case ERR_CLIENT_ALREADY_PRESENT:

            printMsg("Failed HELLO attempt with dupilicate username. Responding \
                      with message ERR CLIENT ALREADY PRESENT");

            if (!clientExistsFD(recvr))
                error("client (sender) does not exist");
            msg1.hdr.type = ERR_CLIENT_ALREADY_PRESENT;
            strcpy(msg1.hdr.src, SERVER_NAME);
            strcpy(msg1.hdr.dst, pheader->src);
            msg1.hdr.len = 0;
            msg1.hdr.msgID = 0;
            msg1.data = NULL;
            ml->msg = msg1;
            ml->tl = NULL;
            if (cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend == NULL) {
                cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend = ml;
            } else {
                MessageList last = getTail(cli_arr[indOfCliFD(recvr, cli_arr)].msgToSend);
                last->tl = ml;
            }
            break;

        case ERR_CANNOT_DELIVER:

            printMsg("ERROR: client does not exist, message cannot be delivered to \
                      recipient. Responding to sender with error message.");

            if (!clientExistsFD(recvr))
                error("client (sender/reciever) does not exist");
            msg1.hdr.type = ERR_CANNOT_DELIVER;
            strcpy(msg1.hdr.src, SERVER_NAME);
            strcpy(msg1.hdr.dst, pheader->src);
            msg1.hdr.len = 0;
            msg1.hdr.msgID = pheader->msgID;
            msg1.data = NULL;
            ml->msg = msg1;
            ml->tl = NULL;
            if (cli_arr[indOfCliFD(recvr, temps_arr)].msgToSend == NULL) {
                cli_arr[indOfCliFD(recvr, temps_arr)].msgToSend = ml;
            } else {
                MessageList last =
                     getTail(temps_arr[indOfCliFD(recvr, temps_arr)].msgToSend);
                last->tl = ml;
            }
            break;

        default:
            error("server error: sending wrong message type");
    }
}

/* makes new client in array: clients */
void newClient(int newcli_fd, Header* pheader, ClientArr clients) {

    Client newcli;
    int i;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if (clients[i].fd == EMPTY_FD) {
            newcli.fd = newcli_fd;
            strcpy(newcli.id, pheader->src);
            newcli.msgToSend = NULL;
            clients[i] = newcli;
            return;
        }
    }

    printMsg("failed to find client -- removing socket");
    remClient(newcli_fd, cli_arr);
}

/* initializes the client lists so that they contain only empty clients */
void initCliLists() {

    int i;

    for (i = 0; i < MAX_NCLIENTS; i++){
        cli_arr[i] = EMPTY_CLIENT;
        temps_arr[i] = EMPTY_CLIENT;
    }
}

/* gets index of specific client in clients array */
int indOfCliFD(int cli_fd, ClientArr clients) {

    int i;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if (clients[i].fd == cli_fd)
            return i;
    }
    return 0;
}

/* makes a byte array of null-terminated client id strings */
/* allocates memory for it which is no mroe than 400 bytes per call */
/* returns a CliListBare structure. */
CliListBare makeClientList() {

    int i;
    bool first = true;
    CliListBare list;
    bzero(clbuf, DATA_MAX_SIZE);
    unsigned int delta = 0;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if (cli_arr[i].fd != EMPTY_FD) {
            /* adding 1 to the length of the string makes sure we null-terminate
               the clients id string */
            memcpy(clbuf + delta, cli_arr[i].id, strlen(cli_arr[i].id) + 1);
            delta = delta + strlen(cli_arr[i].id) + 1;
        }
    }

    list.len = delta;
    list.data = malloc(delta);
    assert(list.data);
    memcpy(list.data, clbuf, delta);
    return list;
}

/* gets tail of messagelist */
MessageList getTail(MessageList ml) {

    if (ml == NULL)
        error("called getTail on NULL MessageList");

    while (ml->tl != NULL) {
        ml = ml->tl;
    }

    return ml;
}

/* looks up a client's file descriptor per its ClientID.
   throws an error if we can't find it since we should never call this on
   a client that doesn't exist */
int ID2FD(ClientID clid, ClientArr clients) {

    int i;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if (strcmp(clients[i].id, clid) == 0) {
            return clients[i].fd;
        }
    }

    error("clientID to client fd error: not found");
    return -1; // will segfault but we will never get here... so that's fine
}

/* reverses the endianness of a byte string of length nBytes */
/* we use char* because it is a convenient way to manipulate
   the byte string */
void revEndian(char* input, unsigned int nBytes) {

    int i, j;
    char temp;

    for (i = 0, j = nBytes - 1; j > i; i++, j--) {
        temp = input[i];
        input[i] = input[j];
        input[j] = temp;
    }
}


/* writes messages attached to clients in both cli_arr (official clients)
   or messages attached to clients in temps_arr (clients who have tried to
   use the server with a nonunique clientID and who will get error messages
   before we delete their connection.) */
void writeMsgs(int dst_fd) {

    // find the client in the appropriate array
    // send the first message in the stack to to the client
    // if the message is a terminal error message (only 7)
    Message msg;
    MessageList freedom;
    int i, n, len;

    for (i = 0; i < MAX_NCLIENTS; i++) {
        if ((cli_arr[i].fd == dst_fd) && (cli_arr[i].msgToSend != NULL)) {

            bzero(packet, PACK_SIZE);
            msg = cli_arr[i].msgToSend->msg;
            freedom = cli_arr[i].msgToSend;
            cli_arr[i].msgToSend = cli_arr[i].msgToSend->tl;
            len = msg.hdr.len;
            msg.hdr.type = htons(msg.hdr.type);
            msg.hdr.len = htonl(msg.hdr.len);
            msg.hdr.msgID = htonl(msg.hdr.msgID);
            memcpy((void *) &packet, (void *) &msg.hdr, HDR_SIZE);
            memcpy((void *) &packet[HDR_SIZE], (void *) msg.data, len);
            n = write(dst_fd, packet, (HDR_SIZE + len));
            if (msg.data != NULL)
                free(msg.data);
            free(freedom);

            if (n == -1) {
                printMsg("Error detected on write to socket. Checking errno.");
                // TODO check errno
            } else if (n < (HDR_SIZE + len)) {
                printMsg("Didn't write full message -- overflow occured somewhere");
            } else if ((n > (HDR_SIZE + len))) {
                printMsg("I didn't know this was possible?");
            }

            printMsg("Successfully wrote message to cli_arr client!");

        } else if ((temps_arr[i].fd == dst_fd) && (temps_arr[i].msgToSend != NULL)) {

            bzero(packet, PACK_SIZE);
            msg = temps_arr[i].msgToSend->msg;
            freedom = temps_arr[i].msgToSend;
            temps_arr[i].msgToSend = temps_arr[i].msgToSend->tl;
            len = msg.hdr.len;
            msg.hdr.type = htons(msg.hdr.type);
            msg.hdr.len = htonl(msg.hdr.len);
            msg.hdr.msgID = htonl(msg.hdr.msgID);
            memcpy((void *) &packet, (void *) &msg.hdr, HDR_SIZE);
            memcpy((void *) &packet[HDR_SIZE], (void *) msg.data, len);
            n = write(dst_fd, packet, (HDR_SIZE + len));
            if (msg.data != NULL)
                free(msg.data);
            free(freedom);

            if (msg.hdr.type == ERR_CLIENT_ALREADY_PRESENT) {
                remClient(dst_fd, temps_arr);
            }

            if (n == -1) {
                printMsg("Error detected on write to socket. Checking errno.");
                // TODO: chekc errno
            } else if (n < (HDR_SIZE + len)) {
                printMsg("Didn't write full message -- overflow occured somewhere");
            } else if ((n > (HDR_SIZE + len))) {
                printMsg("I didn't know this was possible?");
            }

            printMsg("Successfully wrote message to temps_arr client!");
        }
    }
}
