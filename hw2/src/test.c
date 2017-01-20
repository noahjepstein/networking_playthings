#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void printout(char* s);

int main(int argc, char* argv[]) {

    // char t0[40];
    // char t1[20] = "DATA";
    // bzero(t0, 40);
    // t0[0] = (char)65;
    // t0[1] = (char)66;
    // memcpy(t0+2, t1, strlen(t1) + 1);
    // printf("should say ABDATA: %s\n", t0);
    //

    char* lol = "lol";

    printout(("my string %s", lol));

    return 0;
}

void printout(char* s) {
    printf("%s\n", s);
}
