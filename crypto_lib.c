// Requires 'shared_mem' which it will read/encrypt

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

// Buffers to store encrypted and decrypted data separately
static char encrypted_mem[SHM_SIZE] = {0}; // This holds encrypted data
static char decrypted_mem[SHM_SIZE] = {0}; // This holds decrypted data

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
    printk(KERN_INFO "\n");

    return 0; // Encryption done
}

// Decrypt whatever is currently stored in encrypted_mem
static int decrypt_shared_memory(void) {
    if (strlen(encrypted_mem) == 0) { // Nothing to decrypt
        printk(KERN_ERR "No encrypted data available to decrypt\n");
        return -EINVAL;
    }

    // RSA Key Setup - same as above
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
