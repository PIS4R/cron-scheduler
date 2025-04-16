//
// Created by michalpisarski on 9/12/24.
//

#ifndef LOG_H

typedef enum {
    LOG_LEVEL_MIN,
    LOG_LEVEL_STANDARD,
    LOG_LEVEL_MAX
} log_level_t;

void log_message(log_level_t level, const char *message);
void* dump_state(void* arg);
void* change_log_level(void* arg); //int sig, siginfo_t *info, void *contex
void* toggle_logging(void* arg); //int sig


void init_signal_handlers();
void open_log_file(const char *filename);
void close_log_file();

#define LOG_H

#endif //LOG_H
