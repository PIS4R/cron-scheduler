#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <signal.h>
#include <string.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include "log.h"
#include <semaphore.h>
#include <stdatomic.h>

volatile sig_atomic_t current_log_level = LOG_LEVEL_STANDARD;
volatile sig_atomic_t new_current_log_level = LOG_LEVEL_STANDARD;

volatile sig_atomic_t logging_enabled = 1;
FILE *log_file = NULL;

pthread_mutex_t file_mutex = PTHREAD_MUTEX_INITIALIZER;

sem_t sem1, sem2, sem3;
pthread_t tid0, tid1, tid2;


void log_message(log_level_t level, const char *message) {
    if (logging_enabled && level <= current_log_level){
        if (log_file) {
            if(level <= current_log_level)
            {
                time_t now = time(NULL);
                char *timestamp = ctime(&now);
                timestamp[strlen(timestamp) - 1] = '\0';

                fprintf(log_file, "[%s] [%d] [%s]\n",
                    timestamp, current_log_level, message);
                fflush(log_file);
            }
        }
    }
}

void dump_state_handler(int signo, siginfo_t* info, void* other)
{
    sem_post(&sem1);
}

void* dump_state(void* arg) { //int sig, siginfo_t *info, void *context

    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGRTMIN);
    pthread_sigmask(SIG_SETMASK, &set, NULL);

    while(1)
    {

        sem_wait(&sem1);

        char filename[256];
        snprintf(filename, sizeof(filename), "state_dump_%ld.txt", time(NULL));
        FILE *dump_file = fopen(filename, "w");
        if (dump_file) {
            fprintf(dump_file, "Stan wewnętrzny aplikacji" );
            fclose(dump_file);
            log_message(LOG_LEVEL_STANDARD, "Wygenerowano plik dump stanu.");
        }
    }
}

void change_log_level_handler(int signo, siginfo_t* info, void* other)
{
    atomic_store(&new_current_log_level, info->si_value.sival_int);
    sem_post(&sem2);
}

void* change_log_level(void* arg) { //int sig, siginfo_t *info, void *context

    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGRTMIN + 1);
    pthread_sigmask(SIG_SETMASK, &set, NULL);
    while(1)
    {
        sem_wait(&sem2);

        if(new_current_log_level != current_log_level){
            if (new_current_log_level >= LOG_LEVEL_MIN && new_current_log_level <= LOG_LEVEL_MAX){
                atomic_store(&current_log_level, new_current_log_level);
            }
        }
        char msg[64];
        snprintf(msg, sizeof(msg), "Zmieniono poziom logowania na %d", new_current_log_level);
        log_message(LOG_LEVEL_MIN, msg);
    }
}

void toggle_logging_handler(int signo, siginfo_t* info, void* other)
{
    sem_post(&sem3);
}

void* toggle_logging(void* arg) { //int sig

    sigset_t set;
    sigfillset(&set);
    sigdelset(&set, SIGRTMIN + 2);
    pthread_sigmask(SIG_SETMASK, &set, NULL);

    while(1)
    {
        sem_wait(&sem3);

        switch ((int) atomic_load(&logging_enabled))
        {
        case  1:
            log_message(LOG_LEVEL_STANDARD, "Logowanie wyłączone.");
            atomic_store(&logging_enabled, 0);
            break;
        case 0:
            atomic_store(&logging_enabled, 1);
            log_message(LOG_LEVEL_STANDARD, "Logowanie włączone.");
            break;
        }
    }
}

void init_signal_handlers() {
    struct sigaction sa_dump, sa_toggle, sa_log_level;

    sem_init(&sem1, 0, 1);
    sem_init(&sem2, 0, 1);
    sem_init(&sem3, 0, 1);
    sem_wait(&sem1);
    sem_wait(&sem2);
    sem_wait(&sem3);

    pthread_create(&tid0, NULL, dump_state, NULL);
    pthread_create(&tid1, NULL, change_log_level, NULL);
    pthread_create(&tid2, NULL, toggle_logging, NULL);

    sigset_t set;
    sigfillset(&set);

    // sygnał dumpowania stanu
    sa_dump.sa_flags = SA_SIGINFO;
    sa_dump.sa_sigaction = dump_state_handler;
    sa_dump.sa_mask = set;
    //sigemptyset(&sa_dump.sa_mask);
    sigaction(SIGRTMIN, &sa_dump, NULL);

    // sygnał przełączania poziomu
    memset(&sa_log_level, 0, sizeof (sa_log_level));
    sa_log_level.sa_flags = SA_SIGINFO;
    sa_log_level.sa_sigaction = change_log_level_handler;
    sa_log_level.sa_mask = set;
    //sigemptyset(&sa_log_level.sa_mask);
    sigaction(SIGRTMIN + 1, &sa_log_level, NULL);

    // sygnał włączania/wyłączania
    sa_toggle.sa_flags = 0;
    sa_toggle.sa_sigaction = toggle_logging_handler; //sa_handler
    sa_toggle.sa_mask = set;
    //sigemptyset(&sa_toggle.sa_mask);
    sigaction(SIGRTMIN + 2, &sa_toggle, NULL);

    sa_dump.sa_handler = SIG_IGN;
    sa_log_level.sa_handler = SIG_IGN;
    sa_toggle.sa_handler = SIG_IGN;

    for(int i = SIGRTMIN+3; i <= SIGRTMAX; i++)
    {
        sigaction(i, &sa_dump, NULL);
        sigaction(i, &sa_log_level, NULL);
        sigaction(i, &sa_toggle, NULL);
    }

    log_message(LOG_LEVEL_MIN, "INIT_SIGNAL_HANDLERS: log signals initialized");
}

void open_log_file(const char *filename) {
    pthread_mutex_lock(&file_mutex);
    if (log_file) {
        fclose(log_file);
    }

    log_file = fopen(filename, "a+");
    if(log_file == NULL){
        log_message(LOG_LEVEL_MIN, "OPEN_LOG_FILE: opening log file failed");
    }
    pthread_mutex_unlock(&file_mutex);
}

void close_log_file() {
    pthread_mutex_lock(&file_mutex);

    pthread_join(tid0, NULL);
    pthread_join(tid1, NULL);
    pthread_join(tid2, NULL);

    if (log_file) {
        fclose(log_file);
        log_file = NULL;
    }
    pthread_mutex_unlock(&file_mutex);

    sem_destroy(&sem1);
    sem_destroy(&sem2);
    sem_destroy(&sem3);
    pthread_mutex_destroy(&file_mutex);

}
