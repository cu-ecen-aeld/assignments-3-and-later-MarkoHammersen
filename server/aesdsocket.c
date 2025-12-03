#define _POSIX_C_SOURCE 200112L

/* ---------------------------------------------------------------------------
 * Rewritten aesdsocket-style server (logic preserved)
 *
 * Changes:
 *  - Replaced BSD SLIST with manual singly-linked list
 *  - Renamed functions to snake_case
 *  - Clarified variable names
 *  - Updated comments
 *  - Wrapped debug prints in #ifdef DEBUG
 *  - Reordered includes
 * -------------------------------------------------------------------------*/

/* --- Standard C headers --- */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* --- POSIX / system headers --- */
#include <unistd.h>
#include <fcntl.h>
#include <signal.h>
#include <pthread.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <sys/ioctl.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <syslog.h>

/* --- Project headers --- */
#include "../aesd-char-driver/aesd_ioctl.h"

/* Configuration */
#define USE_AESD_CHAR_DEVICE

#ifndef USE_AESD_CHAR_DEVICE
#define VARFILE_PATH "/var/tmp/aesdsocketdata"
#else
#define VARFILE_PATH "/dev/aesdchar"
#endif

#define BUFFER_SIZE 1024

/* -------------------------------------------------------------------------
 * Globals
 * ----------------------------------------------------------------------*/
int server_socket_fd = -1;
pthread_mutex_t data_mutex;
int exit_signal_flag = 0;

/* Manual singly linked list of clients */
struct client_entry {
    pthread_t thread;
    int client_fd;
    char client_ip[INET_ADDRSTRLEN];
    int thread_done;
    struct client_entry *next;
};

/* Head pointer for client list */
struct client_entry *client_list_head = NULL;

/* -------------------------------------------------------------------------
 * Forward declarations
 * ----------------------------------------------------------------------*/
static void close_all_resources(void);
static void handle_exit_signal(int signum);
static void handle_timer_signal(int signum);
static void init_signal_handlers(void);
#ifndef USE_AESD_CHAR_DEVICE
static void init_periodic_timer(void);
#endif
static void daemonize_process(void);
static void server_socket_init(void);

static void* client_thread_main(void* arg);
static int socket_to_file(int client_fd, FILE* data_file);
static int file_to_socket(int client_fd, FILE* data_file);
static int parse_ioctl_seekto(const char *str, unsigned int *x, unsigned int *y);

/* -------------------------------------------------------------------------
 * Implementation
 * ----------------------------------------------------------------------*/

/* Parse "AESDCHAR_IOCSEEKTO:x,y" */
static int parse_ioctl_seekto(const char *str, unsigned int *x, unsigned int *y)
{
    const char *prefix = "AESDCHAR_IOCSEEKTO:";
    size_t len = strlen(prefix);

    if (strncmp(str, prefix, len) != 0)
        return 0;

    return sscanf(str + len, "%u,%u", x, y) == 2;
}

/* Signal handler for SIGINT / SIGTERM */
void handle_exit_signal(int signum)
{
    (void)signum;
    exit_signal_flag = 1;
}

/* Timer signal handler (non-char-device mode) */
void handle_timer_signal(int signum)
{
    (void)signum;
#ifndef USE_AESD_CHAR_DEVICE
    time_t now;
    struct tm *tm_info;
    char buf[128];
    int fd;

    time(&now);
    tm_info = localtime(&now);

    strftime(buf, sizeof(buf), "timestamp:%a, %d %b %Y %T %z\n", tm_info);

    pthread_mutex_lock(&data_mutex);
    fd = open(VARFILE_PATH, O_WRONLY | O_APPEND | O_CREAT, 0640);
    write(fd, buf, strlen(buf));
    pthread_mutex_unlock(&data_mutex);

    close(fd);
#endif
}

/* Register signal handlers */
void init_signal_handlers(void)
{
    struct sigaction sa;

    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0;

    sa.sa_handler = handle_exit_signal;
    sigaction(SIGINT, &sa, NULL);
    sigaction(SIGTERM, &sa, NULL);

    sa.sa_handler = handle_timer_signal;
    sigaction(SIGALRM, &sa, NULL);
}

#ifndef USE_AESD_CHAR_DEVICE
/* Create 10-second periodic timer */
void init_periodic_timer(void)
{
    timer_t timer_id;
    struct itimerspec itval = {0};

    itval.it_value.tv_sec = 10;
    itval.it_interval.tv_sec = 10;

    timer_create(CLOCK_REALTIME, NULL, &timer_id);
    timer_settime(timer_id, 0, &itval, NULL);
}
#endif

/* Detach and run as daemon */
void daemonize_process(void)
{
    pid_t pid = fork();

    if (pid < 0) {
        close_all_resources();
        exit(EXIT_FAILURE);
    }
    if (pid > 0) {
        close_all_resources();
        exit(EXIT_SUCCESS);
    }

    setsid();
    chdir("/");
    close(STDIN_FILENO);
    close(STDOUT_FILENO);
    close(STDERR_FILENO);
}

/* Initialize server socket */
void server_socket_init(void)
{
    struct addrinfo hints, *res;
    int optval = 1;

    server_socket_fd = socket(PF_INET, SOCK_STREAM, 0);
    if (server_socket_fd < 0) {
        close_all_resources();
        exit(EXIT_FAILURE);
    }

    setsockopt(server_socket_fd, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval));

    memset(&hints, 0, sizeof(hints));
    hints.ai_family   = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags    = AI_PASSIVE;

    if (getaddrinfo(NULL, "9000", &hints, &res) != 0) {
        close_all_resources();
        exit(EXIT_FAILURE);
    }

    if (bind(server_socket_fd, res->ai_addr, res->ai_addrlen) != 0) {
        close_all_resources();
        freeaddrinfo(res);
        exit(EXIT_FAILURE);
    }

    freeaddrinfo(res);
}

/* Thread routine: handle a single client */
void* client_thread_main(void* arg)
{
    struct client_entry *client = arg;
    FILE* data_file;

    pthread_mutex_lock(&data_mutex);
    data_file = fopen(VARFILE_PATH, "w+");

    socket_to_file(client->client_fd, data_file);
    file_to_socket(client->client_fd, data_file);

    fclose(data_file);
    pthread_mutex_unlock(&data_mutex);

    client->thread_done = 1;
    return NULL;
}

/* Receive data and write to file (handle ioctl command) */
int socket_to_file(int client_fd, FILE* data_file)
{
    char buf[BUFFER_SIZE + 1];
    int n;
    struct aesd_seekto seek;

    do {
        n = recv(client_fd, buf, BUFFER_SIZE, 0);
        buf[n] = '\0';

        if (parse_ioctl_seekto(buf, &seek.write_cmd, &seek.write_cmd_offset)) {
#ifdef DEBUG
            fprintf(stderr, "ioctl: %u %u\n", seek.write_cmd, seek.write_cmd_offset);
#endif
            ioctl(fileno(data_file), AESDCHAR_IOCSEEKTO, &seek);
        } else {
#ifdef DEBUG
            fprintf(stderr, "to file: %s\n", buf);
#endif
            fwrite(buf, 1, n, data_file);
            fflush(data_file);
        }

    } while (buf[n - 1] != '\n');

    return 1;
}

/* Read file contents and send to client */
int file_to_socket(int client_fd, FILE* data_file)
{
    char buf[BUFFER_SIZE + 1];
    int n;

    while ((n = fread(buf, 1, BUFFER_SIZE, data_file)) > 0) {
        buf[n] = '\0';
#ifdef DEBUG
        fprintf(stderr, "from file: %s\n", buf);
#endif
        send(client_fd, buf, n, 0);
    }

    return 1;
}

/* Close all system resources */
void close_all_resources(void)
{
    if (server_socket_fd != -1)
        close(server_socket_fd);

#ifndef USE_AESD_CHAR_DEVICE
    remove(VARFILE_PATH);
#endif

    pthread_mutex_destroy(&data_mutex);
    closelog();
}

/* -------------------------------------------------------------------------
 * Main
 * ----------------------------------------------------------------------*/
int main(int argc, char** argv)
{
    int daemon_mode = (argc == 2 && strcmp(argv[1], "-d") == 0);

    pthread_mutex_init(&data_mutex, NULL);

    openlog(NULL, 0, LOG_USER);
    init_signal_handlers();
    server_socket_init();

    if (daemon_mode)
        daemonize_process();

#ifndef USE_AESD_CHAR_DEVICE
    init_periodic_timer();
#endif

    listen(server_socket_fd, 1024);

    while (1) {

        /* Reap finished threads */
        struct client_entry *prev = NULL, *cur = client_list_head;

        while (cur) {
            if (cur->thread_done) {
                pthread_join(cur->thread, NULL);
                close(cur->client_fd);

                syslog(LOG_INFO, "Closed connection from %s", cur->client_ip);

                if (prev)
                    prev->next = cur->next;
                else
                    client_list_head = cur->next;

                struct client_entry *to_free = cur;
                cur = cur->next;
                free(to_free);
                continue;
            }
            prev = cur;
            cur = cur->next;
        }

        /* Accept new client */
        struct sockaddr_in client_addr;
        socklen_t addr_len = sizeof(client_addr);

        int new_fd = accept(server_socket_fd, (struct sockaddr*)&client_addr, &addr_len);
        if (new_fd < 0) {
            if (exit_signal_flag)
                break;
            continue;
        }

        char ip[INET_ADDRSTRLEN];
        inet_ntop(AF_INET, &client_addr.sin_addr, ip, sizeof(ip));
        syslog(LOG_INFO, "Accepted connection from %s", ip);

        /* Allocate new list node */
        struct client_entry *new_node = calloc(1, sizeof(struct client_entry));
        new_node->client_fd = new_fd;
        strcpy(new_node->client_ip, ip);
        new_node->thread_done = 0;
        new_node->next = client_list_head;
        client_list_head = new_node;

        pthread_create(&new_node->thread, NULL, client_thread_main, new_node);
    }

    /* Final cleanup: join & free all clients */
    struct client_entry *cur = client_list_head;
    while (cur) {
        pthread_join(cur->thread, NULL);
        close(cur->client_fd);
        syslog(LOG_INFO, "Closed connection from %s", cur->client_ip);

        struct client_entry *next = cur->next;
        free(cur);
        cur = next;
    }

    close_all_resources();
    return EXIT_SUCCESS;
}
