///<summary> 
/// Reader program for the user-space. Its function is take data from kernel space and output it. 
/// It consists of a parent thread and two child processes. Parent thread continously reads from device while 
/// the two child processes, one for console output and one for log-file, display the messages.
///<summary>

#include <stdio.h>
#include <stdlib.h>
#include <fcntl.h>    // File control operations, open()
#include <unistd.h>   // read(), close()
#include <pthread.h>  // Threading
#include <string.h>   // String manipulation
#include <sys/ioctl.h> //for ioctl
#include "message.h"

#define IOCTL_GET_SHM_SIZE _IOR(42, 0, int)
#define IOCTL_SET_SHM_SIZE _IOW(42, 1, int)
#define IOCTL_GET_READER_COUNT _IOR(42, 2, int)
#define IOCTL_GET_CURRENT_BUFFER_SIZE _IOR(42, 3, int)

#define DEVICE_PATH "/dev/ipc_device"
#define LOG_FILE_PATH "/tmp/reader_log.txt" // macro for path to log file

char buffer[4096]; //shared buffer for data read from device

pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex for shared buffer
pthread_cond_t data_available = PTHREAD_COND_INITIALIZER; // conditional variable for when data is avaliable

int string_size;

// Array to store unique hash values of messages, preventing duplication
// idk what word to use other than exhausted
int64_t exhausted_hashes[100]; 
int hash_count = 0;
pthread_mutex_t hash_check_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex for shared buffer

//IOCTL
void get_device_info(int fd) {
    int value;

    ioctl(fd, IOCTL_GET_SHM_SIZE, &value);
    printf("IOCTL: Shared Memory Size: %d \n", value);

    ioctl(fd, IOCTL_GET_READER_COUNT, &value);
    printf("IOCTL: Reader Count: %d \n", value);
}

/*
https://stackoverflow.com/questions/48748121/c-pthreads-parent-child-process

https://www.geeksforgeeks.org/thread-functions-in-c-c/

Lecture 8, Lecture 9, Lecture 10
*/

/* https://medium.com/@joshuaudayagiri/linux-system-calls-read-a9ce7ed33827 */

void initialise_hash_array() {
    for (int i = 0; i < 100; i++) {
        exhausted_hashes[i] = -1;  // Use -1 as "empty" marker
    }
}

int has_seen_hash(int64_t hash_to_check) {
    pthread_mutex_lock(&hash_check_mutex); 
    for (int i = 0; i < 100; i++) { // 100 = length of exhausted_hashes
        // If the hash is found in the array, return 1
        if (exhausted_hashes[i] == hash_to_check) {
            printf("Read message with hash: %ld\n", ((struct message_data*)buffer)->unique_hash);
            pthread_mutex_unlock(&hash_check_mutex); 
            return 1;  // Message has already been printed
        }
    }

    // If we haven't returned yet, it meens the hash hasn't been seen
    exhausted_hashes[hash_count] = hash_to_check; // Add the hash to the array
    hash_count++;

    if (hash_count == 100) {
        hash_count = 0; // Reset the array
    }

    pthread_mutex_unlock(&hash_check_mutex); 
    return 0;  // Hash not seen before
}

// Parent thread continuously reads data from the device
// Parent thread continuously reads data from the device
void* reader_thread(void* arg) {
    int fd = open(DEVICE_PATH, O_RDONLY);
    if (fd == -1) {
        perror("Failed to open device");
        return NULL;
    }

    while (1) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer));

        if (bytes_read > 0) {
            pthread_mutex_lock(&buffer_mutex);
            struct message_data* msg = (struct message_data*)buffer;
            printf("Received message with hash: %ld\n", msg->unique_hash);

            if (!has_seen_hash(msg->unique_hash)) {
                pthread_cond_broadcast(&data_available);
            }
            pthread_mutex_unlock(&buffer_mutex);
        }

        sleep(1);
    }

    close(fd);
    return NULL;
}

void set_shm_size(int new_size) {
    int fd = open(DEVICE_PATH, O_WRONLY);
    if (fd == -1) {
        perror("Failed to open device for writing");
        return;
    }

    ioctl(fd, IOCTL_SET_SHM_SIZE, &new_size);
    printf("IOCTL: Shared memory size set to %d \n", new_size);

    close(fd);
}


// Console writer thread prints data to console
// Console writer thread prints data to console
void* console_writer_thread(void* arg) {
    while (1) {
        pthread_mutex_lock(&buffer_mutex);
        pthread_cond_wait(&data_available, &buffer_mutex);

        struct message_data* msg = (struct message_data*)buffer;

        printf("|| Console | %d | Writer PID: %d || %s\n",
               msg->timestamp, msg->writer_pid, msg->message);

        pthread_mutex_unlock(&buffer_mutex);
    }
    return NULL;
}

/* https://community.ptc.com/t5/Customization/Write-log-file/td-p/642638 */

// Log writer thread: Writes data to a log file
void* log_writer_thread(void* arg) {
    FILE* log_file = fopen(LOG_FILE_PATH, "a"); //open log file in append mode
    if (log_file == NULL) {
        perror("failed to open log file");
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(&buffer_mutex); 
        pthread_cond_wait(&data_available, &buffer_mutex);  

        struct message_data* msg = (struct message_data*)buffer;

        fprintf(
            log_file,
            "|| Log | %d | Writer PID: %d || %s\n",
            msg->timestamp, msg->writer_pid, msg->message
        );

        /* https://how.dev/answers/what-is-fflush-in-c */

        fflush(log_file);  // immediately write to the log file/disk

        pthread_mutex_unlock(&buffer_mutex);
    }

    fclose(log_file);
    return NULL; // exits
}

int main(int argc, char* argv[]) {
    initialise_hash_array(); // Initialise the hash array
    // takes input from the console to set the shm. example: sudo ./reader 1024
    if (argc == 2) {
        int new_size = atoi(argv[1]); //converts from string to int
        set_shm_size(new_size); //callin ioctl to set custom shm
    }

    pthread_t reader_tid, console_writer_tid, log_writer_tid; //thread identifiers

    // Start the threads
    //Reader
    //// creating threads with default attributes and args
    if (pthread_create(&reader_tid, NULL, reader_thread, NULL) != 0) { 
        perror("Failed to create reader thread");
        return -1;
    }
    //Console writer
    if (pthread_create(&console_writer_tid, NULL, console_writer_thread, NULL) != 0) {
        perror("Failed to create console writer thread");
        return -1;
    }
    //Log writer
    if (pthread_create(&log_writer_tid, NULL, log_writer_thread, NULL) != 0) {
        perror("Failed to create log writer thread");
        return -1;
    }

    // Wait for threads to finish 
    pthread_join(reader_tid, NULL); // _join so program doesnt end before threads
    pthread_join(console_writer_tid, NULL);
    pthread_join(log_writer_tid, NULL);

    return 0;
}
