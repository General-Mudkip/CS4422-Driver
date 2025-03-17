// The struct for all the metadata the emssage contains (and the message itself)
struct message_data {
    pid_t writer_pid;       // Process ID of the writer
    time_t timestamp;       // When the message was created
    size_t message_length;  // Length of the actual message's content
    char message[];         // La message
};
