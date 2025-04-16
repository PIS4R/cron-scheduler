//#define __USE_XOPEN
#define _XOPEN_SOURCE       /* See feature_test_macros(7) */
#define _POSIX_C_SOURCE 200809L
//#define _POSIX_C_SOURCE 199309L
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <sys/timerfd.h>
#include <sys/types.h>
#include <fcntl.h>
#include <signal.h>
#include <time.h>
#include <sys/wait.h>
#include <mqueue.h>
#include <errno.h>
#include <sys/types.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdint.h>
#include <threads.h>
#include <spawn.h>
#include <string.h>
#include "log.h"

#define MAX_TASKS 100
#define MAX_COMMAND_LEN 256
#define MAX_WORD_IN_COMMAND 256
#define MAX_CLIENT_QUEUE_NAME 128
#define MQ_NAME "/cron_queue"
#define MQ_CLIENT_NAME "/cron_client_queue"
#define LOG_FILE_FILENAME "app.log"
//#define PID_FILE "/tmp/cron_daemon.pid"

struct task_t
{
    time_t start_time; // Czas rozpoczęcia
    int interval; // Interwał (w sekundach, 0 jeśli jednorazowo)
    char command[MAX_COMMAND_LEN]; // Komenda do uruchomienia
    int id; // Identyfikator zadania
    int active; // Flaga aktywności
    //mqd_t client_mq; // Kolejka komunikatow ustanowiona z klientem ktory zaplanowal task
    timer_t timer;
};

enum request_type {ADD_TASK, LIST, CANCEL};

struct client_request
{
    enum request_type req_type;
    time_t start_time;
    int interval;
    char command[MAX_COMMAND_LEN];
    long client_id;
    long task_id;
    char client_queue_name[MAX_CLIENT_QUEUE_NAME];
};

struct task_t **tasks = NULL;
int task_count = 0;
pthread_mutex_t task_mutex = PTHREAD_MUTEX_INITIALIZER;

void destroy_tasks()
{
    for(int i = 0; i < task_count; i++){
        free( *(tasks+i));
    }
    free(tasks);
}

time_t parse_absolute_time(const char* time_str)
{
    struct tm tm = {0};
    strptime(time_str, "%Y-%m-%d %H:%M:%S", &tm);
    return mktime(&tm);
}

int parse_to_absolute_time(time_t raw_time, char* time_str)
{
    struct tm time_info;

    if (localtime_r(&raw_time, &time_info) == NULL){
        return -1; // Error in getting local time
    }
    //printf("time tm: %d-%02d-%02d %02d:%02d:%02d\n", time_info.tm_year + 1900, time_info.tm_mon + 1, time_info.tm_mday,
    //    time_info.tm_hour, time_info.tm_min, time_info.tm_sec);

    if (strftime(time_str, 256, "%Y-%m-%d %H:%M:%S", &time_info) == 0){
        return -1;
    }
    return 0;
}

void* timer_thread(void* arg)
{

    void *nothing = NULL;
    struct task_t *task = (struct task_t *) arg;

    char *args[100];
    int arg_count = 0;
    char *state = task->command;
    char command_copy[MAX_COMMAND_LEN];
    memcpy(command_copy, task->command, sizeof(task->command));

    char* token = strtok_r(command_copy, " ", &state);
    while (token != NULL && arg_count < MAX_WORD_IN_COMMAND) {
        args[arg_count++] = token;
        token = strtok_r(NULL, " ", &state);
    }
    args[arg_count] = NULL;

    pid_t child_pid;
    int err = posix_spawnp(&child_pid, args[0], NULL, NULL, args, NULL);
    if(err == -1){
        perror("posix_spawn");
        log_message(LOG_LEVEL_MIN, "timer_thread: FUNCTION posix_spawnp RETURNED -1");
    }
    if (task->interval == 0) {
        pthread_mutex_lock(&task_mutex);
        task->active = 0;
        timer_delete(task->timer);
        log_message(LOG_LEVEL_MAX, "timer_thread: THREAD ENDED");
        pthread_mutex_unlock(&task_mutex);
    }

    return nothing;
}

void add_task(struct client_request req)
{
    pthread_mutex_lock(&task_mutex);
    log_message(LOG_LEVEL_MAX, "STARTED CREATING NEW TASK");

    struct task_t *new_local_task = malloc(sizeof(struct task_t));
    if(new_local_task == NULL)
    {
        perror("malloc err");
        log_message(LOG_LEVEL_MIN, "ADD_TASK: function malloc returned NULL");
        pthread_mutex_unlock(&task_mutex);
        destroy_tasks();
        exit(EXIT_FAILURE);
    }
    new_local_task->start_time = req.start_time;
    new_local_task->interval = req.interval;
    memcpy(new_local_task->command, req.command, sizeof(req.command));
    new_local_task->id = task_count + 1;
    new_local_task->active = 1;
    log_message(LOG_LEVEL_MAX, "NEW TASK ALLOCATED");

    struct sigevent timer_event;
    timer_event.sigev_notify = SIGEV_THREAD;
    timer_event.sigev_notify_function = (void(*)(union sigval))timer_thread;
    timer_event.sigev_value.sival_ptr = new_local_task;
    timer_event.sigev_notify_attributes = NULL;
    timer_create(CLOCK_REALTIME, &timer_event, &new_local_task->timer);
    log_message(LOG_LEVEL_MAX, "NEW TIMER CREATED");

    struct itimerspec value;
    value.it_value.tv_sec = new_local_task->start_time - time(NULL);
    value.it_value.tv_nsec = 0;
    value.it_interval.tv_sec = new_local_task->interval;
    value.it_interval.tv_nsec = 0;
    timer_settime(new_local_task->timer, 0, &value, NULL);
    log_message(LOG_LEVEL_MAX, "NEW TIMER SET UP");

    struct task_t** tmp = realloc(tasks, sizeof(struct task_t*) * (task_count + 1));
    if(tmp == NULL)
    {
        perror("malloc err");
        log_message(LOG_LEVEL_MIN, "ADD_TASK: function realloc returned NULL");
        pthread_mutex_unlock(&task_mutex);
        destroy_tasks();
        exit(EXIT_FAILURE);
    }
    tasks = tmp;
    *(tasks + task_count) = new_local_task;
    log_message(LOG_LEVEL_MAX, "NEW TASK ASSIGNED TO MEMORY");

    static __thread char buffer[8192] = "";
    static __thread char time_str[64] = "";
    parse_to_absolute_time(req.start_time, time_str);
    snprintf(buffer, sizeof(buffer), "Task %s will execute at %s", req.command, time_str);

    mqd_t reply_to_client_mq = mq_open(req.client_queue_name, O_WRONLY);
    if(reply_to_client_mq == -1){
        perror("mq_open");
        log_message(LOG_LEVEL_MIN, "ADD_TASK: function mq_open returned -1");
    }
    int err = mq_send(reply_to_client_mq, buffer, sizeof(buffer), 2);
    if(err == -1){
        perror("mq_send");
        log_message(LOG_LEVEL_MIN, "ADD_TASK: function mq_send returned -1");
    }
    err = mq_close(reply_to_client_mq);
    if(err == -1){
        perror("mq_close");
        log_message(LOG_LEVEL_MIN, "ADD_TASK: function mq_close returned -1");
    }
    else
    {
        log_message(LOG_LEVEL_MAX, "ADD_TASK: replay_to_client_queue closed");
    }


    char tmp_time_str[64];
    char message_to_log[128];
    parse_to_absolute_time(new_local_task->start_time, tmp_time_str);
    snprintf(message_to_log, sizeof(message_to_log), "NEW TASK SUCCESSFULLY ADDED [%d] [%d] [%s] [%s]", new_local_task->id, new_local_task->interval, tmp_time_str, new_local_task->command);
    log_message(LOG_LEVEL_STANDARD, message_to_log);

    task_count++;
    pthread_mutex_unlock(&task_mutex);
}

void cancel_task(struct client_request req)
{
    pthread_mutex_lock(&task_mutex);
    log_message(LOG_LEVEL_MAX, "CANCEL_TASK: Started cancelling task");

    char buffer[4096] = "";
    for (int i = 0; i < task_count; i++)
    {
        if ( (*(tasks + i))->id == req.task_id && ( (*(tasks + i))->active ) )
        {
            (*(tasks + i))->active = 0;
            timer_delete((*(tasks + i))->timer);
            snprintf(buffer, sizeof(buffer), "Task %ld was canceled\n", req.task_id);
            log_message(LOG_LEVEL_STANDARD, "CANCEL_TASK: user successfully cancelled task");
            break;
        }
    }
    if (strlen(buffer) == 0)
    {
        snprintf(buffer, sizeof(buffer), "This ID is not connected to any task\n");
        log_message(LOG_LEVEL_STANDARD, "CANCEL_TASK: user's task to cancel was not active");
    }

    mqd_t reply_to_client_mq = mq_open(req.client_queue_name, O_WRONLY);
    if(reply_to_client_mq == -1){
        perror("mq_open");
        log_message(LOG_LEVEL_MIN, "CANCEL_TASK: function mq_open returned -1");
    }
    int err = mq_send(reply_to_client_mq, buffer, sizeof(buffer), 2);
    if(err == -1){
        perror("mq_send");
        log_message(LOG_LEVEL_MIN, "CANCEL_TASK: function mq_send returned -1");
    }
    err = mq_close(reply_to_client_mq);
    if(err == -1){
        perror("mq_close");
        log_message(LOG_LEVEL_MIN, "CANCEL_TASK: function mq_close returned -1");
    }
    pthread_mutex_unlock(&task_mutex);
}

void list_tasks(struct client_request req)
{
    pthread_mutex_lock(&task_mutex);
    log_message(LOG_LEVEL_MAX, "LIST_TASKS: Started listing tasks");

    char buffer[4096] = "";
    for (int i = 0; i < task_count; i++)
    {
        if ((*(tasks + i))->active)
        {
            char time_str[128]; //static __thread
            char task_info_line[256]; //static __thread
            parse_to_absolute_time((*(tasks + i))->start_time, time_str);
            snprintf(task_info_line, sizeof(task_info_line), "ID: %d, Command: %s, Start Time: %s, Interval: %d\n",
                     (*(tasks + i))->id, (*(tasks + i))->command, time_str, (*(tasks + i))->interval);

            strcat(buffer, task_info_line);
        }
    }
    if (strlen(buffer) == 0){
        strcat(buffer, "No active tasks at this moment\n");
        log_message(LOG_LEVEL_STANDARD, "LIST_TASKS: There was no tasks to list");
    } else{
        log_message(LOG_LEVEL_STANDARD, "LIST_TASKS: Tasks listed");
    }

    mqd_t reply_to_client_mq = mq_open(req.client_queue_name, O_WRONLY);
    if(reply_to_client_mq == -1){
        perror("mq_open");
        log_message(LOG_LEVEL_MIN, "LIST_TASKS: function mq_open returned -1");
    }
    int err = mq_send(reply_to_client_mq, buffer, sizeof(buffer), 2);
    if(err == -1){
        perror("mq_send");
        log_message(LOG_LEVEL_MIN, "LIST_TASKS: function mq_send returned -1");
    }
    err = mq_close(reply_to_client_mq);
    if(err == -1){
        perror("mq_close");
        log_message(LOG_LEVEL_MIN, "LIST_TASKS: function mq_close returned -1");
    }

    pthread_mutex_unlock(&task_mutex);
}


void* mq_listener(void* arg)
{
    mqd_t mq = *(mqd_t*)arg;
    struct client_request buffer;

    while (1)
    {
        ssize_t bytes_read = mq_receive(mq, (char *) &buffer, sizeof(buffer), NULL);
        if(bytes_read == -1)
        {
            perror("mq_receive");
            log_message(LOG_LEVEL_MIN, "function mq_recive returned -1");
        }
        if (bytes_read >= 0)
        {
            log_message(LOG_LEVEL_MAX, "SERVER GOT NEW REQUEST");
            struct client_request client_request;

            client_request.client_id = buffer.client_id;
            strcpy(client_request.client_queue_name, buffer.client_queue_name);
            //printf("client_queue_name: %s\n", client_request.client_queue_name);
            client_request.task_id = buffer.task_id;
            client_request.start_time = buffer.start_time;
            client_request.interval = buffer.interval;
            strcpy(client_request.command, buffer.command);
            //printf("mq_listener client_request.command: %s\n", client_request.command);
            client_request.req_type = buffer.req_type;
            //printf("Request Type: %d\n", client_request.req_type);

            if (client_request.req_type == LIST){
                list_tasks(client_request);
            }
            else if (client_request.req_type == CANCEL){
                cancel_task(client_request);
            }
            else{
                add_task(client_request);
            }
        }
    }
}

void handle_server_break(int sig) {
    log_message(LOG_LEVEL_MIN, "SERVER STOPPED");
    mq_unlink(MQ_NAME);
    pthread_mutex_destroy(&task_mutex);
    destroy_tasks();
    close_log_file();
    exit(0);
}

// Funkcja uruchamiania serwera jako demona
int start_server()
{
    mqd_t mq_clients_requests;
    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 10;
    attr.mq_msgsize = sizeof(struct client_request);
    attr.mq_curmsgs = 0;


    // otwarcie kolejki z poleceniami do servera
    mq_clients_requests = mq_open(MQ_NAME, O_CREAT | O_RDONLY | O_EXCL, 0644, &attr);
    if (mq_clients_requests == -1 && errno == EEXIST)
        return 1; //client

    //server

    // Inicjalizacja systemu logowania
    open_log_file(LOG_FILE_FILENAME);
    init_signal_handlers();
    log_message(LOG_LEVEL_MIN, "SERVER STARTED");

    // jesli jakies dziecko zrobi exit to ignoruj to *zalozenie ze system to posprzata*
    signal(SIGCHLD, SIG_IGN);

    // Ustaw uchwyt sygnału SIGINT (CTRL + C)
    signal(SIGINT, handle_server_break);

    // EDIT: The above condition will block the parent process until the child completes.
    // When the child exits, the SIGCHLD signal is sent to the parent, and by default it is ignored.
    // However, according to POSIX.1-2001, if you explicitly set the signal handler for SIGCHLD to SIG_IGN,
    // then the system will automatically clean up the child,
    // with the caveat that the parent process will not know anything about the exit status of the child.
        // https://unix.stackexchange.com/questions/542904/child-process-killing-itself-but-becoming-zombie

    pthread_t mq_tid, log_tid;

    pthread_create(&mq_tid, NULL, mq_listener, (void*)&mq_clients_requests);

    printf("Serwer cron uruchomiony.\n");
    pthread_join(mq_tid, NULL);

    mq_close(mq_clients_requests);
    mq_unlink(MQ_NAME);

    return 0;
}


void  start_client(int argc, char* argv[])
{
    if (argc < 2)
    {
        printf("Użycie: %s <-r|-a|-list|-cancel> [start_time] [interval] [command]\n", argv[0]);
        exit(1);
    }

    struct mq_attr attr;
    attr.mq_flags = 0;
    attr.mq_maxmsg = 12;
    attr.mq_msgsize = 8192;
    attr.mq_curmsgs = 0;

    char client_queue_name[256];
    sprintf(client_queue_name, "/client_%d", getpid());

    // sudo sh -c 'echo 512 > /proc/sys/fs/mqueue/msg_max'
    // kolejka do czytania komunikatow od servera
    const mqd_t receive_reply_mq = mq_open(client_queue_name, O_CREAT | O_RDONLY, 0777, &attr); //, 0666, &attr | O_RDONLY
    if(receive_reply_mq == -1)
    {
        if(errno == EMFILE)
        {
            perror("EMFILE");
        }
        else if(errno == ENOSPC)
        {
            perror("ENOSPC");
        }
        perror("mq_open");
    }

    static __thread struct client_request request;
    strcpy(request.client_queue_name, client_queue_name);

    if (strcmp(argv[1], "-list") == 0)
    {
        request.req_type = LIST;
    }
    else if (strcmp(argv[1], "-cancel") == 0)
    {
        if (argc != 3)
        {
            printf("Użycie: %s -cancel <task_id>\n", argv[0]);
            exit(1);
        }
        request.req_type = CANCEL;
        request.task_id = atoi(argv[2]);
    }
    else if (strcmp(argv[1], "-r") == 0)
    {
        if (argc < 5)
        {
            printf("Użycie: %s -r <start_time_in_seconds> <interval> <command>\n", argv[0]);
            exit(1);
        }
        char command[256];
        strcpy(command, argv[4]);

        for (int i = 5; i < argc; i++)
        {
            strcat(command, " ");
            strcat(command, argv[i]);
        }

        request.req_type = ADD_TASK;
        request.start_time = (time_t)(time(NULL) + atol(argv[2]));
        request.interval = (long)(atol(argv[3]));

        strcpy(request.command, command);
    }
    else if (strcmp(argv[1], "-a") == 0)
    {
        if (argc < 5)
        {
            printf("Użycie: %s -a <absolute_time> <interval> <command>\n", argv[0]);
            exit(1);
        }
        request.req_type = ADD_TASK;

        char absolute_time_string[256];
        strcpy(absolute_time_string, argv[2]);
        strcat(absolute_time_string, " ");
        strcat(absolute_time_string, argv[3]);

        request.interval = (long)(atol(argv[4]));

        char command[256];
        strcpy(command, argv[5]);
        for (int i = 6; i < argc; i++)
        {
            strcat(command, " ");
            strcat(command, argv[i]);
        }
        strcpy(request.command, command);

        char time_tmp[100] = "";
        parse_to_absolute_time(parse_absolute_time(absolute_time_string), time_tmp);
        request.start_time = (time_t)parse_absolute_time(absolute_time_string);
    }

    mqd_t mq_send_request = mq_open(MQ_NAME, O_WRONLY);
    mq_send(mq_send_request, (const char *) &request, sizeof(request), 0);
    mq_close(mq_send_request);

    // ODPOWIEDZ Z SERVERA
    char reply[8192];
    int err = mq_receive(receive_reply_mq, reply, sizeof(reply), NULL);
    if(err == -1)
    {
        if(errno == EAGAIN)
        {
            perror("EAGAIN");
        }
        else if(errno == EBADF)
        {
            perror("EBADF");
        }
        else if(errno == EINTR)
        {
            perror("EINTR");
        }
        else if(errno == EINVAL)
        {
            perror("EINVAL");
        }
        else if(errno == EMSGSIZE)
        {
            perror("EMSGSIZE");
        }
        else if(errno == ETIMEDOUT)
        {
            perror("ETIMEDOUT");
        }
    }
    printf("%s\n", reply);


    mq_close(receive_reply_mq);
    mq_unlink(client_queue_name);
}

int main(int argc, char* argv[])
{
    if (start_server() == 1)
        start_client(argc, argv);
    // mq_unlink(MQ_NAME);

    return 0;
}

