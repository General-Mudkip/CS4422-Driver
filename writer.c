
///<summary> Writer program for the user-space. Its function is to take input from the console and write it to the kernal. 

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>  // File control operations, open()
#include <unistd.h> // write(), close()
#include <string.h>  // String manipulation

#define DEVICE_PATH "/dev/ipc_device"

/* https://www.geeksforgeeks.org/command-line-arguments-in-c-cpp/ */

//Take messages from console
//argc is no. of arguments and argv is an array of string for the arguments
int main(int argc, char *argv[]) {  
    if (argc < 2) { //means message was not provided
        printf("Format: %s \"your message\" \n", argv[0]); 
        printf("Provide a message."); 
        return 1;
    }

    // Open device for writing
    int fd = open(DEVICE_PATH, O_WRONLY); // file descriptor that stores what open() returns
    if (fd == -1) { 
        perror("Failed to open device"); 
        return 1;
    }

    /* https://www.quora.com/How-does-the-write-function-work-in-C-Can-you-explain-this-function */

    // Write messages to device 
    ssize_t bytes_written = write(fd, argv[1], strlen(argv[1])); //writes the message (argv[1]) to device
    if (bytes_written < 0) {
        perror("Write failed");
    } else {
        printf("Data written to device: %zu bytes\n", bytes_written);
    }

    close(fd); 
    return 0;
}
