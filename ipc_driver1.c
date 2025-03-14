#include <linux/kernel.h> // core kernel func and macros
#include <linux/init.h> // module init and exit
#include <linux/fs.h> // File operations
#include <linux/uaccess.h> // for copy_to_user()
#include <linux/mutex.h>
#include <linux/semaphore.h>
#include <linux/crypto.h>   // Encryption
#include <linux/slab.h>      // Memory allocation
#include <linux/mm.h>        // Shared memory
#include <linux/cdev.h> // registering character devices
#include <linux/proc_fs.h>  // for proc_create and remove_proc_entry


#define DEVICE_NAME "Simple IPC" 
#define MAJOR_DEVICE_NUMBER 42
#define MINOR_DEVICE_NUMBER 0

#define PROC_FILENAME "ipc_stats"

#define SHM_SIZE 1024 // Shared Memory Size
#define MAX_READER_COUNT 4 // The maximum amount of readers at any one time


MODULE_LICENSE("GPL");
MODULE_AUTHOR("");
MODULE_DESCRIPTION("A simple IPC driver");
MODULE_VERSION("1.0");

static struct class *ipc_class = NULL;
static struct device *ipc_device = NULL;


static char *shared_mem;
static int data_written = 0;
static int readers_remaining = 3; // 1 parent reader + 2 child threads

static struct proc_dir_entry *proc_file;

//Proc File stats variables 
static unsigned long userspace_accesses = 0;
static unsigned long total_bytes_read = 0;
static unsigned long total_bytes_write = 0;
static unsigned long reads_count = 0;
static unsigned long writes_count = 0;
static size_t max_read = 0;
static size_t min_read = SIZE_MAX; 
unsigned long avg_bytes_read = 0;
unsigned long avg_bytes_written = 0;


// https://0xax.gitbooks.io/linux-insides/content/SyncPrim/linux-sync-5.html
// https://oscourse.github.io/slides/semaphores_waitqs_kernel_api.pdf
static DEFINE_SEMAPHORE(rw_sem, MAX_READER_COUNT); // Semaphore for read/write


// Function prototypes
static int device_open(struct inode *inode, struct file *file);
static int device_closed(struct inode *inode, struct file *file);
static ssize_t device_read(struct file *file, char __user *user_buffer, size_t len, loff_t *offset);
static ssize_t device_write(struct file *file, const char __user *user_buffer, size_t len, loff_t *offset);
static ssize_t stats_read(struct file *file, char __user *buffer, size_t count, loff_t *offset);

static int proc_read = 0;

static int ipc_proc_init(void);
static void ipc_proc_exit(void); 

// File operation structure
static struct file_operations fops = {
    .open = device_open,
    .release = device_closed,
    .read = device_read,
    .write = device_write,
};

// Proc file operation structue
static const struct proc_ops proc_fops = {
    .proc_read = stats_read,
};

// Intialising
static int __init device_init(void) {
    int retval;
    retval = register_chrdev(MAJOR_DEVICE_NUMBER, DEVICE_NAME, &fops);  // register the device

    if (retval == 0) {
        printk("dev_testdr registered to major number %d and minor number %d\n", MAJOR_DEVICE_NUMBER, MINOR_DEVICE_NUMBER);
    } else {
        printk("Could not register dev_testdr\n");
        return retval;
    }

    //Create device class
    ipc_class = class_create("ipc_class");
    if (IS_ERR(ipc_class)) {
        unregister_chrdev(MAJOR_DEVICE_NUMBER, DEVICE_NAME);
        printk(KERN_ALERT "Failed to create device class\n");
        return PTR_ERR(ipc_class);
    }

    // Create device node - this makes the device appear in /dev/
    ipc_device = device_create(ipc_class, NULL, MKDEV(MAJOR_DEVICE_NUMBER, 0), NULL, "ipc_device");
    if (IS_ERR(ipc_device)) {
        class_destroy(ipc_class);
        unregister_chrdev(MAJOR_DEVICE_NUMBER, DEVICE_NAME);
        printk(KERN_ALERT "Failed to create device\n");
        return PTR_ERR(ipc_device);
    }

    //semaphore
    sema_init(&rw_sem, MAX_READER_COUNT);  // 1 for single reader

    // allocating dynamic memory for shared memory using kmalloc
    shared_mem = kmalloc(SHM_SIZE, GFP_KERNEL);
    if (!shared_mem) {
        printk(KERN_ALERT "memory allocation failed\n");
        return -ENOMEM;  // out of memory
    }

    ipc_proc_init();

    printk(KERN_INFO "Device registered with major number %d\n", MAJOR_DEVICE_NUMBER);
    return 0; 

}

/* https://devarea.com/linux-kernel-development-creating-a-proc-file-and-interfacing-with-user-space/ */

//Initialising proc file
static int __init ipc_proc_init(void) {
    proc_file = proc_create("ipc_stats", 0444, NULL, &proc_fops); // read only proc file
    if (!proc_file) {
        printk(KERN_ALERT "Failed to create proc file \n");
        return -ENOMEM;
    }
    printk(KERN_INFO "Proc file created \n");
    return 0;
}

// Cleaning up the device
static void __exit device_exit(void) {
    device_destroy(ipc_class, MKDEV(MAJOR_DEVICE_NUMBER, 0)); // Remove the device
    class_destroy(ipc_class); // Remove the device class
    unregister_chrdev(MAJOR_DEVICE_NUMBER, DEVICE_NAME);  // Unregister the device

    kfree(shared_mem); // free the memory
    printk(KERN_INFO "Device unregistered\n");

    ipc_proc_exit();
}

//Clean-up proc
static void __exit ipc_proc_exit(void) {
    remove_proc_entry( "ipc_stats", NULL);
}


// Open func
static int device_open(struct inode *inode, struct file *file) {

    userspace_accesses++;

    printk(KERN_INFO "Device opened\n");
    return 0;
}

// Close func
static int device_closed(struct inode *inode, struct file *file) {

    userspace_accesses++;

    printk(KERN_INFO "Device closed\n");
    return 0;
}

// Read
static ssize_t device_read(struct file *file, char __user *user_buffer, size_t len, loff_t *offset) {
    size_t bytes_to_read = min(len, SHM_SIZE);

    if (len > max_read) {
        max_read = len;   // update if current read is more than previous max
    }
    if (len < min_read) {
        min_read = len;  
    }

    //update proc file stats
    userspace_accesses++;
    reads_count++;         
    total_bytes_read += len;


    if (data_written == 0) { // check for data
        printk(KERN_INFO "No data available to read\n");
        return 0; 
    } 

    if (down_interruptible(&rw_sem)) { //semaphore lcoked to prevent race con
        printk(KERN_ALERT "Semaphore down interruptible failed\n");
        return -EINTR;
    }

    //encrypt data

    printk(KERN_INFO "Reader acquired semaphore\n");


    if (copy_to_user(user_buffer, shared_mem, bytes_to_read)) { 
        printk(KERN_ERR "Failed to copy data to uesr space\n");
        up(&rw_sem);  // to ensure its released or else it gets stuck
        return -EFAULT;
    }

    printk(KERN_INFO "Device read %zu bytes\n", bytes_to_read); // log device logging upon read

    // readers now read in cycles hence decrement after finish reading
    readers_remaining--; 

    //CHECK THIS!!!!
    if (readers_remaining <= 0) {  
        data_written = 0;  // reset only after all readers have read
        readers_remaining = 3;  // reset for the next read cycle
    }

    up(&rw_sem);
    printk(KERN_INFO "Reader released semaphore\n");;

    return bytes_to_read;
}

// Write
static ssize_t device_write(struct file *file, const char __user *user_buffer, size_t len, loff_t *offset) {
    size_t bytes_to_write = min(len, SHM_SIZE);

    //proc file stats
    userspace_accesses++;
    writes_count++;         
    total_bytes_write += len; 

    // Decrement the semaphore by the max amount of readers.
    // This ensure that when the writer is writing, no readers are reading.

    for (int i = 0; i < MAX_READER_COUNT; i++) {
        if (down_interruptible(&rw_sem)) {
            printk(KERN_ALERT "Semaphore down interruptible failed\n");
            return -EINTR;
        }

    }

    if (copy_from_user(shared_mem, user_buffer, bytes_to_write)) {
        up(&rw_sem);
        return -EFAULT;
    }

    data_written = 1;

    // encrypt data

    printk(KERN_INFO "Device wrote %zu bytes\n", bytes_to_write);

    memset(shared_mem + bytes_to_write, 0, SHM_SIZE - bytes_to_write); //clearing buffer

    for (int i = 0; i < MAX_READER_COUNT; i++) {
        up(&rw_sem);
    }

    return bytes_to_write;
}

ssize_t stats_read(struct file *file, char __user *buffer, size_t count, loff_t *offset) {
    char *stats;
    int len = 0;

    if (proc_read) { //avoid reading the stats again if already read
        return 0;
    }

    stats = kmalloc(1024, GFP_KERNEL); // dynamic memory allocation for the stats
    if (stats == NULL) {
        printk(KERN_ERR "Failed to allocate memory\n");
        return -ENOMEM;  
    }

    // Calculating averages
    if (reads_count > 0) {
        avg_bytes_read = total_bytes_read / reads_count;
    } 

    if (writes_count > 0) {
        avg_bytes_written = total_bytes_write / writes_count;
    } 

    // Format the stats to display
    size_t formatted_len; 
    formatted_len = snprintf(stats, 1024,
                             "IPC Device Statistics:\n \n"
                             "1. Total user-space accesses: %lu \n"
                             "2. Total read operations: %lu \n"
                             "3. Total write operations: %lu \n"
                             "4. Total bytes read: %lu bytes \n"
                             "5. Total bytes written: %lu bytes \n"
                             "6. Average bytes per read: %lu bytes \n"
                             "7. Average bytes per write: %lu bytes \n"
                             "8. Maximum read size: %lu bytes\n"
                             "9. Minimum read size: %lu bytes\n \n"
                             "::::::::::::::::::::::::::::::::::::::::::::\n \n",
                             userspace_accesses, 
                             reads_count,
                             writes_count,
                             total_bytes_read,
                             total_bytes_write,
                             avg_bytes_read,
                             avg_bytes_written,
                             max_read,
                             min_read);

    //return formated stats to user
    if (copy_to_user(buffer, stats + *offset, formatted_len - *offset)) {
        printk(KERN_ERR "Failed to copy stats\n");
        kfree(stats);
        return -EFAULT;
    }

    proc_read = 1;  

    kfree(stats);
    return len;
}

module_init(device_init); // initialising func
module_exit(device_exit); // exit func
