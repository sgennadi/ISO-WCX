/* Compile only the AES-256 ECB primitive into the WCX core. */
#define AES256 1
#define ECB 1
#define CBC 0
#define CTR 0
#include "aes.c"
