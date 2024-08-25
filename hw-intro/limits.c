#include <stdio.h>
#include <sys/resource.h>
#include <errno.h>

int main() {
    struct rlimit lim;
    if( getrlimit(RLIMIT_STACK, &lim) != 0) 
        fprintf(stderr, "Getting limit error.");
    printf("stack size: %ld\n", lim.rlim_cur);
    if( getrlimit(RLIMIT_NPROC, &lim) != 0) 
        fprintf(stderr, "Getting limit error.");
    printf("process limit: %ld\n", lim.rlim_cur);
    if( getrlimit(RLIMIT_NOFILE, &lim) != 0) 
        fprintf(stderr, "Getting limit error.");
    printf("max file descriptors: %ld\n", lim.rlim_cur);
    return 0;
}
