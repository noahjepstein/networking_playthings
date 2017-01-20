#!/bin/bash

echo "Enter name of server host."
read HOSTNAME
echo "Enter port number for testing."
read PORTNUM

# FILEDIR = "files/"

for i in `seq 1 6`
do
    echo "File index: " $i

    for j in 'seq 1 9'
    do
        ./a3-client $HOSTNAME $PORTNUM files/t$i.data $j 0
    done
done
