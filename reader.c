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
#include <time.h>

#define DEVICE_PATH "/dev/ipc_device"
#define LOG_FILE_PATH "/tmp/reader_log.txt" // macro for path to log file
#define BUFFER_SIZE 1024 

char buffer[BUFFER_SIZE]; //shared buffer for data read from device

pthread_mutex_t buffer_mutex = PTHREAD_MUTEX_INITIALIZER; // mutex for shared buffer

pthread_cond_t data_available = PTHREAD_COND_INITIALIZER; // conditional variable for when data is avaliable

/*
https://stackoverflow.com/questions/48748121/c-pthreads-parent-child-process

https://www.geeksforgeeks.org/thread-functions-in-c-c/

Lecture 8, Lecture 9, Lecture 10
*/

// Parent thread continuously reads data from the device
void* reader_thread(void* arg) {
    int fd = open(DEVICE_PATH, O_RDONLY);
    if (fd == -1) {
        perror("Failed to open device");
        return NULL;
    }

    while (1) {
        ssize_t bytes_read = read(fd, buffer, sizeof(buffer) - 1);
        if (bytes_read < 0) {
            perror("Failed to read");
            break;
        }

        if (bytes_read > 0) {
            buffer[bytes_read] = '\0';  // null-terminate buffer, prevent garbage data

            pthread_mutex_lock(&buffer_mutex); //restricts access to buffer
            pthread_cond_signal(&data_available); //signals other threads 
            pthread_mutex_unlock(&buffer_mutex); //
        }
    }

    close(fd);
    return NULL;
}

// Console writer thread prints data to console
void* console_writer_thread(void* arg) {
    while (1) {
        pthread_mutex_lock(&buffer_mutex);
        pthread_cond_wait(&data_available, &buffer_mutex);  // waits for condition variable to be signaled
        printf("| Console | %d s | Data read from device: %s\n", (int)time(NULL), buffer);

        pthread_mutex_unlock(&buffer_mutex);
    }

    return NULL;
}

/* https://community.ptc.com/t5/Customization/Write-log-file/td-p/642638 */

// Log writer thread: Writes data to a log file
void* log_writer_thread(void* arg) {
    FILE* log_file = fopen(LOG_FILE_PATH, "a");
    if (log_file == NULL) {
        perror("failed to open log file");
        return NULL;
    }

    while (1) {
        pthread_mutex_lock(&buffer_mutex); 
        pthread_cond_wait(&data_available, &buffer_mutex);  

        // https://stackoverflow.com/questions/11765301/how-do-i-get-the-unix-timestamp-in-c-as-an-int
        fprintf(log_file, "| Log | %d | Data read from device: %s\n", (int)time(NULL), buffer);

        /* https://how.dev/answers/what-is-fflush-in-c */

        fflush(log_file);  // immediately write to the log file/disk

        pthread_mutex_unlock(&buffer_mutex);
    }

    fclose(log_file);
    return NULL; // exits
}

int main() {
    pthread_t reader_tid, console_writer_tid, log_writer_tid; //thread identifiers

    // Start the threads
    //Reader
    if (pthread_create(&reader_tid, NULL, reader_thread, NULL) != 0) { // creating thraeds with default attributes and args
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
