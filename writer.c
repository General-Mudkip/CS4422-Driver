
///<summary> Writer program for the user-space. Its function is to take input from the console and write it to the kernal. 

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>  // File control operations, open()
#include <unistd.h> // write(), close()
#include <string.h>  // String manipulation
#include <sys/ioctl.h> // For accessing ioctl shtuff
#include<time.h> //for readable time
#include "message.h"

#define DEVICE_PATH "/dev/ipc_device"

// Defining the ioctl functions
#define IOCTL_GET_SHM_SIZE _IOR(42, 0, int)

/* https://www.geeksforgeeks.org/command-line-arguments-in-c-cpp/ */


// http://www.cse.yorku.ca/~oz/hash.html
// Hashes a provided string
int64_t hash(unsigned char *str)
{
    int64_t hash = 5381;
    int c;

    while (c = *str++)
        hash = ((hash << 5) + hash) + c; /* hash * 33 + c */

    return hash;
}

//Take messages from console
//argc is no. of arguments and argv is an array of string for the arguments
int main(int argc, char *argv[]) {
    if (argc < 2) { //means message was not provided
        printf("ERROR: No message provided.\n"); 
        printf("Expected format: %s \"your message\" \n", argv[0]); 
        return 1;
    }

    if (argc > 2) {
        printf("ERROR: Too many arguments provided.\n"); 
        printf("Expected format: %s \"your message\" \n", argv[0]); 
        return 1;
    }

    // Open device for writing
    // O_WRONLY == write only
    int fd = open(DEVICE_PATH, O_WRONLY); // file descriptor that stores what open() returns
    if (fd == -1) { 
        perror("Failed to open device"); 
        return 1;
    }

    // Testing ioctl shtuff
    int shm_size;
    if (ioctl(fd, IOCTL_GET_SHM_SIZE, &shm_size) == -1) {
        perror("Failed to get shared memory size");
        close(fd);
        return -1;
    }
    printf("Shared Memory Size: %d\n", shm_size);

    /* https://www.quora.com/How-does-the-write-function-work-in-C-Can-you-explain-this-function */

    size_t message_length = strlen(argv[1]); //Length of the message string
    size_t total_message_size = sizeof(struct message_data) + message_length; //Size of the message struct with metadata

    struct message_data *new_msg = malloc(total_message_size);
    if (!new_msg) {
        perror("Failed to allocate memory for message");
        close(fd);
        return -1;
    }

    // Set la data in the struct (`->` automatically dereferences the pointer)
    new_msg->writer_pid = getpid(); // Process ID of the writer
    new_msg->timestamp = time(NULL); // When the message was created
    new_msg->message_length = message_length; // Length of the actual message's content
    // Just copies the message passed in as the argument to the memory location
    // opf the message in the struct
    memcpy(new_msg->message, argv[1], message_length);

    char timestamp_string[256]; 
    char pid_string[256]; 
    sprintf(timestamp_string, "%ld", new_msg->timestamp); // Converts timestamp to string
    sprintf(pid_string, "%ld", new_msg->writer_pid); // Converts PID to string

    char *to_be_hashed = malloc(strlen(timestamp_string) + strlen(pid_string) + message_length + 1); // +1 for null terminator
    if (!to_be_hashed) {
        perror("Failed to allocate memory for hash string");
        close(fd);
        return -1;
    }

    to_be_hashed[0] = '\0'; //empty string
    to_be_hashed = strcat(to_be_hashed, timestamp_string);
    to_be_hashed = strcat(to_be_hashed, pid_string);
    to_be_hashed = strcat(to_be_hashed, new_msg->message);
    printf("Pre hash: %s \nPost hash: %d\n", to_be_hashed, hash(to_be_hashed));

    // Absolutely cooked way of hashing it but sure
    new_msg->unique_hash = hash(to_be_hashed); // Used for identifying unique messages

    // Write messages to device 
    ssize_t bytes_written = write(fd, new_msg, total_message_size); //writes the message  to device

    if (bytes_written < 0) {
        perror("Write failed");
    } else {
        printf("Data written to device successfully.");
    }

    close(fd); 
    return 0;
}
