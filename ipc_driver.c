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
#include <linux/ioctl.h> // for the ioctl commands
#include "message.h"

#define DEVICE_NAME "Simple IPC" 
#define MAJOR_DEVICE_NUMBER 42
#define MINOR_DEVICE_NUMBER 0
#define SHM_SIZE 4096 

#define PROC_FILENAME "ipc_stats"

#define MAX_READER_COUNT 4 // The maximum amount of readers at any one time (arbitrary)

// https://embetronicx.com/tutorials/linux/device-drivers/ioctl-tutorial-in-linux/
#define IOCTL_GET_SHM_SIZE _IOR(MAJOR_DEVICE_NUMBER, 0, int) // get shared memory (/buffer) size
#define IOCTL_SET_SHM_SIZE _IOW(MAJOR_DEVICE_NUMBER, 1, int) // set shared memory (/buffer) size
#define IOCTL_GET_READER_COUNT _IOR(MAJOR_DEVICE_NUMBER, 2, int) // get max reader count
#define IOCTL_GET_CURRENT_BUFFER_SIZE _IOR(MAJOR_DEVICE_NUMBER, 3, int) // get the length of the current string in the buffer

MODULE_LICENSE("GPL");
MODULE_AUTHOR("");
MODULE_DESCRIPTION("A simple IPC driver");
MODULE_VERSION("1.0");

static struct class *ipc_class = NULL;
static struct device *ipc_device = NULL;

static size_t shm_size = 1024;

static char encrypted_mem[SHM_SIZE] = {0};  // holds encrypted data
static char decrypted_mem[SHM_SIZE] = {0};  // holds decrypted data


static char *shared_mem;
static int data_written = 0;
static int readers_remaining = 3;

static struct proc_dir_entry *proc_file;

//Proc File stats variables 
static unsigned long userspace_accesses = 0;
static unsigned long total_bytes_read = 0;
static unsigned long total_bytes_write = 0;
static unsigned long reads_count = 0;
static unsigned long writes_count = 0;
static size_t max_written = 0;
static size_t min_written = SIZE_MAX; 
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
static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg);
static long long mod_inverse(long long e, long long phi);
static long long mod_exp(long long base, long long exp, long long mod);


static int proc_read = 0;


static int ipc_proc_init(void);
static void ipc_proc_exit(void); 

// File operation structure
static struct file_operations fops = {
    .open = device_open,
    .release = device_closed,
    .read = device_read,
    .write = device_write,
    .unlocked_ioctl = device_ioctl,
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
    shared_mem = kmalloc(shm_size, GFP_KERNEL);
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

static long device_ioctl(struct file *file, unsigned int cmd, unsigned long arg)
{
    int retval = 0;
    int temp;

    // https://www.geeksforgeeks.org/c-switch-statement/
    switch (cmd) {
        // Get shared memory size
        case IOCTL_GET_SHM_SIZE:
            temp = shm_size;
            if (copy_to_user((int __user *)arg, &temp, sizeof(temp))) {
                retval = -EFAULT;
            }
            break;

        // Set shared memory size
        case IOCTL_SET_SHM_SIZE:
            if (copy_from_user(&temp, (int __user *)arg, sizeof(temp))) {
                retval = -EFAULT;
            } else {
                // Ensure temp is between reasonable bounds
                // Upper bound was picked arbitrarily.
                if (temp > 0 && temp <= 1024 * 10) { 
                    shm_size = temp;
                    kfree(shared_mem);
                    shared_mem = kmalloc(shm_size, GFP_KERNEL);
                } else {
                    // All the various error numbers: ( a lot )
                    // https://www.man7.org/linux/man-pages/man3/errno.3.html
                    retval = -EINVAL;
                }
            }
            break;

        // Get max number of readers
        case IOCTL_GET_READER_COUNT:
            temp = MAX_READER_COUNT;
            if (copy_to_user((int __user *)arg, &temp, sizeof(temp))) {
                retval = -EFAULT;
            }
            break;

        // Get size of current stored string
        case IOCTL_GET_CURRENT_BUFFER_SIZE:
            temp = strlen(shared_mem);
            if (copy_to_user((int __user *)arg, &temp, sizeof(temp))) {
                retval = -EFAULT;
            }
            break;

        default:
            retval = -EINVAL;
            break;
    }
    
    return retval;
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
//DECRYPTION FUNCTIONS
// Function that finds the modular inverse using the extended Euclidean algorithm
// Needed for decryption - finds 'd' so that (e * d) % phi = 1
static long long mod_inverse(long long e, long long phi) {
    long long t = 0, newt = 1, r = phi, newr = e;
    while (newr != 0) {
        long long quotient = r / newr;
        long long temp = t;
        t = newt;
        newt = temp - quotient * newt;
        temp = r;
        r = newr;
        newr = temp - quotient * newr;
    }
    return (t < 0) ? t + phi : t; // Make sure result is positive
}
// Decrypt whatever is currently stored in encrypted_mem
static int __decrypt_shared_memory(void) {
    // Don't need to lock the data as we already have the semaphore lock when this is called
    if (strlen(encrypted_mem) == 0) { // Nothing to decrypt
        printk(KERN_ERR "No encrypted data available to decrypt\n");
        return -EINVAL;
    }

    // RSA Key Setup - same as below
    long long p = 61, q = 53;
    long long n = p * q;
    long long phi = (p - 1) * (q - 1);
    long long e = 17;
    long long d = mod_inverse(e, phi);

    printk(KERN_INFO "Decrypting...\n");

    memset(decrypted_mem, 0, sizeof(decrypted_mem)); // Clear buffer before storing decrypted data
    char temp[6] = {0}; // Temp buffer for extracting encrypted numbers
    int len = strlen(encrypted_mem) / 5; // Each encrypted number is stored as 5 characters

    for (int i = 0; i < len; i++) {
        strncpy(temp, &encrypted_mem[i * 5], 5); // Get one encrypted number
        long long enc_val = simple_strtol(temp, NULL, 10); // Convert back to integer
        decrypted_mem[i] = (char)mod_exp(enc_val, d, n); // Decrypt character
    }

    printk(KERN_INFO "Decrypted Message: %s\n", decrypted_mem);
    return 0; // Decryption done
}

// Generates the keys for RSA encryption
static long string_to_int(const char* str) {
    long result = 0;
    int len = strlen(str);

    // iterate through each character in the provide password
    for (int i = 0; i < len; i++) {
        // Wonky stuff but basically shifts the current int left by 8 bits (length
        // of a char) and then "appends" the current char to the end of the int,
        // since OR'ing the 8 0's with the new character results in the new character
        //
        // In order to convert the character to a number, we cast it to an unsigned char
        result = (result << 8) | (unsigned char)str[i];
    }

    // Ideally we should check if the number is prime, but it's fine for a simple
    // password hashing function
    return result;
}

// Read
static ssize_t device_read(struct file *file, char __user *user_buffer, size_t len, loff_t *offset) {
    size_t bytes_to_read = min(len, shm_size);


    //update proc file stats
    userspace_accesses++;
    reads_count++;


    if (data_written == 0) { // check for data
        // printk(KERN_INFO "No data available to read\n");
        return 0; 
    } 

    if (down_interruptible(&rw_sem)) { //semaphore lcoked to prevent race con
        printk(KERN_ALERT "Semaphore down interruptible failed\n");
        return -EINTR;
    }

    // Don't need to lock the data as we already have the semaphore lock
    // decrypt data
    __decrypt_shared_memory();

    printk(KERN_INFO "Reader acquired semaphore\n");


    if (copy_to_user(user_buffer, shared_mem, bytes_to_read)) { 
        printk(KERN_ERR "Failed to copy data to user space\n");
        up(&rw_sem);  // to ensure its released or else it gets stuck
        return -EFAULT;
    }

    printk(KERN_INFO "Device read %zu bytes\n", bytes_to_read); // log device logging upon read

    // readers now read in cycles hence decrement after finish reading
    readers_remaining--; 

    if (readers_remaining <= 0) {  
        data_written = 0;  // reset only after all readers have read
        readers_remaining = 3;  // reset for the next read cycle
    }

    up(&rw_sem);
    printk(KERN_INFO "Reader released semaphore\n");;

    return bytes_to_read;
}
//  ENCRYPTON FUNCTIONS:
// Function that uses modular exponentiation to compute (base^exp) % mod
// Basically, raises 'base' to the power of 'exp' under modulo 'mod' efficiently.
static long long mod_exp(long long base, long long exp, long long mod) {
    long long result = 1;
    while (exp > 0) {
        if (exp % 2 == 1) // If exponent is odd, multiply by base
            result = (result * base) % mod;
        base = (base * base) % mod; // Square the base
        exp /= 2;
    }
    return result;
}


// Encrypt whatever is currently in shared memory
static int encrypt_shared_memory(void) {
    if (!shared_mem || data_written == 0) { // Check if there's anything to encrypt
        printk(KERN_ERR "No data available in shared memory for encryption\n");
        return -EINVAL;
    }

    // RSA Key Generation - hardcoded for now
    long long p = 61, q = 53; 
    long long n = p * q;
    long long phi = (p - 1) * (q - 1);
    long long e = 17; // Public exponent
    long long d = mod_inverse(e, phi); // Private exponent

    printk(KERN_INFO "Public Key: (e=%lld, n=%lld)\n", e, n);
    printk(KERN_INFO "Private Key: (d=%lld, n=%lld)\n", d, n);

    // Read message from shared memory
    char message[256] = {0};
    strncpy(message, shared_mem, sizeof(message) - 1);

    printk(KERN_INFO "Original Message from Shared Memory: %s\n", message);

    long long encrypted[256];
    int len = strnlen(message, sizeof(message));
    printk(KERN_INFO "Encrypted: ");
    
    memset(encrypted_mem, 0, sizeof(encrypted_mem)); // Clear before storing new encrypted data

    for (int i = 0; i < len; i++) {
        encrypted[i] = mod_exp((long long)message[i], e, n); // Encrypt character
        snprintf(&encrypted_mem[i * 5], 6, "%05lld", encrypted[i]); // Store encrypted as string
        printk(KERN_CONT "%lld ", encrypted[i]);
    }
    printk(KERN_INFO "\nEncrypted Data: %s\n", encrypted_mem);


    return 0; // Encryption done
}

// Write
static ssize_t device_write(struct file *file, const char __user *user_buffer, size_t len, loff_t *offset) {
    size_t bytes_to_write = min(len, shm_size);
    
     if (len > max_written) {
        max_written = len;   // update if current read is more than previous max
    }

    if (len < min_written) {
        min_written = len;  
    }

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
    encrypt_shared_memory();
  

    printk(KERN_INFO "Device wrote %zu bytes\n", bytes_to_write);

    memset(shared_mem + bytes_to_write, 0, shm_size - bytes_to_write); //clearing buffer

    for (int i = 0; i < MAX_READER_COUNT; i++) {
        up(&rw_sem);
    }

    return bytes_to_write;
}



ssize_t stats_read(struct file *file, char __user *buffer, size_t count, loff_t *offset) {
printk(KERN_INFO "stats_read called\n");
    char *stats;
    int len;
    
    
    if (proc_read) {
        proc_read = 0; // Reset for the next read
        return 0; // Signal end of read
    }

    stats = kmalloc(1024, GFP_KERNEL); 
    if (!stats) {
        printk(KERN_ERR "Failed to allocate memory\n");
        return -ENOMEM;  
    }

    // Calculate averages
    
    if (writes_count > 0) {
    avg_bytes_written = total_bytes_write / writes_count;
    }

    total_bytes_read = total_bytes_write;
    
    len = snprintf(stats, 1024,
        "Userspace accesses: %lu\n"
        "Total bytes read: %lu\n"
        "Total bytes written: %lu\n"
        "Reads count: %lu\n"
        "Writes count: %lu\n"
        "Max written: %zu\n"
        "Min written: %zu\n"
        "Avg bytes written: %lu\n",
        userspace_accesses, total_bytes_read, total_bytes_write,
        reads_count, writes_count, max_written, min_written, avg_bytes_written);

    if (copy_to_user(buffer, stats, len)) {
        kfree(stats);
        return -EFAULT;
        
    }
    
     proc_read = 1;

    kfree(stats);
    return len;
}


module_init(device_init); // initialising func
module_exit(device_exit); // exit func
