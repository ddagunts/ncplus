#include <stdio.h>
#include <pthread.h>
static void*f(void*a){puts("thread ok");return 0;}
int main(){pthread_t t;pthread_create(&t,0,f,0);pthread_join(t,0);puts("pthread main ok");return 0;}
