/* tls.h -- see tls.c. MIT licensed, see LICENSE. */
#ifndef PB_TLS_H
#define PB_TLS_H

/* Set up the process-wide canary. Called for you by pb_tls_attach. */
void pb_tls_init(void);

/* Give this thread a bionic-shaped thread pointer. Idempotent. Must be done
 * before the thread runs ANY module code, or the first function with a stack
 * protector faults reading TPIDR_EL0 + 0x28. `who` is for the log only. */
int  pb_tls_attach(const char *who);

/* Release this thread's block. Only for threads that end. */
void pb_tls_detach(void);

int  pb_tls_present(void);

/* Read the guard slot back exactly as the game does. Logs the result. */
int  pb_tls_selftest(void);

#endif
