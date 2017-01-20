/*
    Filename: a1.c
    Author: Noah J. Epstein
    Date Modified: 9.27.2016
    Date Created: 9.25.2016


    This file satisfies assignment 1 for F16: Comp112.

    This file implements a simple HTTP server that can only respond with two
    different resources: a JSON of recent users and info about them, or a
    basic HTML about page.

    The source is self-sufficient in that no files are required to run the
    server except a1.c.

    Compile:  > gcc -g a1.c -lnsl

    Usage:  To run server:
              > ./a.out 9000      # port number (here 9000) is the only argument

             # requesting resource / gives a list of recent users json
             # requesting resource about.html gives a sample html page
             # requesting more than 5 times in 20 seconds returns error 444
             # requesting unknown resources returns error 404
             # methods other than GET return error 405: method not allowed

    Contact: noahjepstein@gmail.com
*/

#include <stdio.h>
#include <assert.h>
#include <stdlib.h>
#include <stdbool.h>
#include <string.h>
#include <unistd.h>
#include <sys/time.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#include <netdb.h>


#define HEADER_OK "HTTP/1.0 200 OK\r\n"
#define ABOUT_HTML "<html><body><h1>Hi, I'm NoahServer.</h1><p>You've been served.</p></body></html>\r\n\r\n"
#define RES404 "HTTP/1.0 404 Not Found\r\n"
#define NF_HTML "<html><body><h1>The page you are looking for does not exist on this server.</h1></body></html>\r\n\r\n"
#define RES405 "HTTP/1.0 405 Method Not Allowed\r\n"
#define ERR_HTML "<html><body><h1>Request Error. Please try again.</h1></body></html>\r\n\r\n"
#define RES_400RATELIM "HTTP/1.0 444 Request Rate Limit Exceeded\r\n\r\n<html><body><h1>RATE LIMIT EXCEEDED.</h1><p>The resource cannot be served becuase you have requested it too many times in too short a time period. Please wait twenty seconds and try again.<p></body><html>\r\n"
#define MAX_USERS 3
#define BUF_SIZE 512
#define RLIMIT_N 5
#define RLIMIT_MS 20000
#define NEW_CLIENT -1


/* type definitions */

// contains info about a particular client
// keeps a circular buffer of their most recent visit times
struct Userinfo {
    char IP_Str[INET_ADDRSTRLEN];
    int nAccesses;
    char *loc;
    unsigned long long visits[RLIMIT_N]; // circular buf of RLIMIT_N recent visit times
    int visHead; // current index in buffer
} init;

// circular buffer of clients
struct Userlist {
    struct Userinfo UIArr[MAX_USERS];
    int head;
} Users;

typedef enum { ABOUT, INDEX, NF, ERR} ReqType;

/* function declarations */
void error(const char *msg);
char* printUsersJSON();
bool rateLimit(char* ip, int client_index);
bool updateRecents(char* ip);
ReqType getRequestType(char* request);
char* makeResponse(ReqType rType, bool isRateLimited);
void initCliList();
int getClientInd(char* ip);
struct Userinfo newClient(char* ip);
char* getClientLoc(char* ip);
long deltaMSec(unsigned long long t1, unsigned long long t2);
char* clientGetReq(char* ip);
char* getCityFromJSON(char* response);
unsigned long long curr_time();


int main(int argc, char* argv[]) {

    int sockfd, newsockfd, portno, n;
    char buffer[BUF_SIZE];
    char cli_ip_str[INET_ADDRSTRLEN];
    char* response;
    ReqType rType;
    bool rateLimit = false;
    struct sockaddr_in serv_addr;
    struct sockaddr_in cli_addr;
    struct UIList * usrlist = NULL;
    struct sockaddr_in* pV4Addr;
    struct in_addr ipAddr;
    bzero((char *) &serv_addr, sizeof(serv_addr));
    socklen_t clilen;

    if (argc < 2) {
        fprintf(stderr,"ERROR, no port provided\n");
        exit(1);
    }
    if (argc > 2) {
        fprintf(stderr, "ERROR, too many arguments provided\n");
        exit(1);
    }

    initCliList();

    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
       error("ERROR opening socket");

    portno = atoi(argv[1]);
    serv_addr.sin_family = AF_INET;
    serv_addr.sin_addr.s_addr = INADDR_ANY;
    serv_addr.sin_port = htons(portno);

    if (bind(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
         error("ERROR on binding");

    listen(sockfd,5);
    clilen = sizeof(cli_addr);

    while (1) {

        // accept new connection
        newsockfd = accept(sockfd,
                    (struct sockaddr *) &cli_addr,
                    &clilen);
        if (newsockfd < 0)
            error("ERROR on accept");

        // get ip from cli_addr
        pV4Addr = (struct sockaddr_in*)&cli_addr;
        ipAddr = pV4Addr->sin_addr;
        inet_ntop(AF_INET, &ipAddr, cli_ip_str, INET_ADDRSTRLEN);

        rateLimit = updateRecents(cli_ip_str); // done, untested

        //read request, determine type
        bzero(buffer, BUF_SIZE);
        n = read(newsockfd, buffer, BUF_SIZE - 1);
        if (n < 0)
            error("ERROR reading from socket");

        rType = getRequestType(buffer); // to be completed

        // respond appropriately (url info they wanted, rate limiting, etc)
        response = makeResponse(rType, rateLimit); // to be completed
        n = write(newsockfd, response, BUF_SIZE);
        if (n < 0)
            error("ERROR writing to socket");

        close(newsockfd);
    }

    close(sockfd);
    return 0;
}


void error(const char *msg) {
    perror(msg);
    exit(1);
}

// returns true if the user should be rate limited
// by checking the difference in utc time between
// this call and the 5th call ago
// if fewer than 5 calls have been made, nothing happens
bool rateLimit(char* ip, int client_index) {

    assert(client_index != NEW_CLIENT);
    struct Userinfo usr = Users.UIArr[(client_index % MAX_USERS)];

    if (usr.nAccesses >= RLIMIT_N) {
        assert(usr.visHead >= 0);
        long delta =
            deltaMSec(usr.visits[(usr.visHead + 1) % RLIMIT_N], curr_time());

        if (delta < RLIMIT_MS) {
            return true;
        }
    }
    return false;
}

long deltaMSec(unsigned long long t1, unsigned long long t2) {
    return (t2 - t1);
}

// gets utc time
unsigned long long curr_time() {

    struct timeval tv;

    gettimeofday(&tv, NULL);

    unsigned long long millisecondsSinceEpoch =
        (unsigned long long)(tv.tv_sec) * 1000 +
        (unsigned long long)(tv.tv_usec) / 1000;

    return millisecondsSinceEpoch;
}

// finds the client int he circular buffer
int getClientInd(char* ip) {

    int i;
    for (i = Users.head; i >= 0; i = i - 1) {
        if (strcmp(Users.UIArr[i % MAX_USERS].IP_Str, ip) == 0) {
            return i;
        }
    }
    return NEW_CLIENT; //stub
}

// initializes the list of clients
void initCliList() {

    // make placeholder user
    init.visHead = 0;
    init.nAccesses = 0;
    int j;

    for (j = 0; j < RLIMIT_N; j = j + 1) {
        init.visits[j] = 0;
    }

    Users.head = 0;
    int i;
    for (i = Users.head; i < MAX_USERS; i = i + 1) {
        Users.UIArr[i] = init;
    }
}

// initializes a new client
struct Userinfo newClient(char* ip) {

    struct Userinfo nc;
    strcpy(nc.IP_Str, ip);
    nc.nAccesses = 1;
    nc.loc = getClientLoc(ip);
    nc.visits[0] = curr_time(); // stub get current time
    nc.visHead = 0;
    return nc;
}

// queries the freegeoip api to get he location of a client. makes
// the actual get requst using clientGetReq.
// parses the city out using getCityFromJSON.
char* getClientLoc(char* ip) {

    char* response = malloc(BUF_SIZE * 5);
    response = clientGetReq(ip);
    char* city = getCityFromJSON(response);
    return city;
}

// parses city from response from freegeip api json
char* getCityFromJSON(char* response) {

    char* buf;
    char* city;
    char* tok;
    buf = strtok(response, "{");
    buf = strtok(NULL, "}");
    tok = strtok(buf, ":");

    while (tok != NULL) {

        tok = strtok(NULL, ",");

        if (strncmp("\"city\":", tok, 7) == 0) {
            strtok(tok, ":");
            city = strtok(NULL, ":");

            if (strcmp("\"\"", city) == 0)
                return "N/A";
            return city;
        }
    }
    return "N/A";
}

// sends get request to get
char* clientGetReq(char* ip) {

    int sockfd, n;
    int portno = 80;
    struct sockaddr_in serv_addr;
    struct hostent *server;
    int bsize = BUF_SIZE * 4;
    char* buffer = malloc(bsize);
    char* hostname = "freegeoip.net";


    sockfd = socket(AF_INET, SOCK_STREAM, 0);
    if (sockfd < 0)
        error("ERROR opening socket");

    server = gethostbyname(hostname);
    if (server == NULL) {
        fprintf(stderr,"ERROR, no such host\n");
        exit(0);
    }

    bzero((char *) &serv_addr, sizeof(serv_addr));
    serv_addr.sin_family = AF_INET;
    bcopy((char *)server->h_addr,
         (char *)&serv_addr.sin_addr.s_addr,
         server->h_length);
    serv_addr.sin_port = htons(portno);
    if (connect(sockfd, (struct sockaddr *) &serv_addr, sizeof(serv_addr)) < 0)
        error("ERROR connecting to freegeoip.net");

    bzero(buffer, bsize);
    snprintf(buffer, bsize - 1,
         "GET /json/%s HTTP/1.0\r\n"
         "Host: %s\r\n"
         "Content-type: application/x-www-form-urlencoded\r\n"
         "Content-length: %d\r\n\r\n", ip, hostname, (unsigned int)bsize);

    n = write(sockfd, buffer, BUF_SIZE);
    if (n < 0)
         error("ERROR writing to socket");
    bzero(buffer, bsize);
    n = read(sockfd, buffer, bsize - 1);
    if (n < 0)
         error("ERROR reading from socket");

    close(sockfd);
    return buffer;
}


// updates list of recent visitors
// returns true if the updated client needs to be rate limited
bool updateRecents(char* ip) {

    bool rateLimClient;
    int client_index = getClientInd(ip);

    // create a new client
    if (client_index == NEW_CLIENT) {

        Users.UIArr[(Users.head + 1) % MAX_USERS] = newClient(ip);
        Users.head = Users.head + 1;

    } else {

        // update an existing client

        /* if ip is in list:
           check if it needs to be rate limited
           pull it out of the middle and
           put it at the beginning of the list and increment its count */
        rateLimClient = rateLimit(ip, client_index);

        struct Userinfo temp = Users.UIArr[(client_index) % MAX_USERS];
        temp.nAccesses = temp.nAccesses + 1;
        temp.visits[(temp.visHead + 1) % RLIMIT_N] = curr_time();
        temp.visHead = temp.visHead + 1;

        int i;

        // shift the buffer if you pulled the client from the middle of the list
        for (i = client_index;
                           (i > Users.head - MAX_USERS) && (i >= 0); i = i - 1){

            Users.UIArr[(i + 1) % MAX_USERS] = Users.UIArr[i % MAX_USERS];
        }
        Users.UIArr[(Users.head + 1) % MAX_USERS] = temp;
        Users.head = Users.head + 1;

    }

    return rateLimClient;
}

// determines the type of the request
// and the resource the request is looking for
ReqType getRequestType(char* request) {

    ReqType rt;

    assert(request != NULL);


    char* line1 = strtok(request, "\r\n");
    char* method = strtok(line1, " ");
    char* resource = strtok(NULL, " ");

    if ((method == NULL) || (resource == NULL)) {
        rt = ERR;
        return rt;
    }

    if ((strcmp(method, "GET") != 0) ||
        (method == NULL) ||
        (resource == NULL) ) { //   || (strcmp(method, "HEAD") == 0)) {

        rt = ERR;

    } else if (strcmp(resource, "/") == 0) {
        rt = INDEX;

    } else if (strcmp(resource, "about.html") == 0) {
        rt = ABOUT;

    } else if (strcmp(resource, "/about.html") == 0) {
        rt = ABOUT;

    } else if (strcmp(resource, "/about.html/") == 0) {
        rt = ABOUT;

    } else {
        rt = NF;
    }
    return rt;
}

// forms the response that we serve based on the response type
// and whether or not the client should be rate limited.
char* makeResponse(ReqType rType, bool isRateLimited) {

    int res_size = BUF_SIZE;
    char* response = malloc(res_size);
    assert(response != NULL);
    bzero(response, res_size);

    switch (rType) {

    case ABOUT:

        snprintf(response, res_size - 1,
            HEADER_OK
            "Content-type: text/html\r\n\r\n"
            "%s", ABOUT_HTML);
        break;

    case INDEX:

        if (isRateLimited) {
            snprintf(response, res_size - 1, RES_400RATELIM);
            break;

        } else {

            free(response);
            response = malloc(BUF_SIZE * 10);
            assert(response != NULL);

            strcat(response, HEADER_OK);
            strcat(response, "Content-type: application/json\r\n\r\n");
            strcat(response, printUsersJSON());
            strcat(response, "\r\n\r\n");
            break;
        }

        break;

    case NF:

        snprintf(response, res_size - 1,
            RES404
            "Content-type: text/html\r\n"
            "Connection: close\r\n\r\n"
            NF_HTML);
        break;

    case ERR:

        snprintf(response, res_size - 1,
            RES405
            "Content-type: text/html\r\n"
            "Connection: close\r\n\r\n"
            ERR_HTML);
        snprintf(response, res_size - 1, RES405);
        break;
    }
    return response;
}

/*  client of makeResponse that prints relevant info in the current client list
    as a json. outputs a json string. */
char* printUsersJSON() {

    char* response = malloc(BUF_SIZE * 5);
    sprintf(response, "[");
    int i;

    for (i = Users.head;
         (i >= 0)
            && ((i > (Users.head - MAX_USERS) ))
            && (Users.UIArr[i % MAX_USERS].nAccesses > 0);
         i = i - 1)
    {
        char* usrstr = malloc(BUF_SIZE);
        if (i != Users.head)
            strcat(response, ",");

        sprintf(usrstr, "{\"IP\":\" %s \",\"Accesses\": %d,\"City\":\" %s \"}",
                          Users.UIArr[i % MAX_USERS].IP_Str, Users.UIArr[i % MAX_USERS].nAccesses,
                          Users.UIArr[i % MAX_USERS].loc);

        strcat(response, usrstr);
    }

    strcat(response, "]");

    return response;
}
