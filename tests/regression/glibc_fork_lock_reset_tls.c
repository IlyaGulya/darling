// A shared object with a dynamic-TLS variable, so dlopen()ing it forces glibc to
// update the TLS slotinfo under GL(_dl_load_tls_lock). Built as tls_mod.so by
// run-glibc-fork-lock-reset.sh.
__thread int tls_counter;
int tls_bump(void) { return ++tls_counter; }
