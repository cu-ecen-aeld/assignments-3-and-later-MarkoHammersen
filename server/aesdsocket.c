#define _POSIX_C_SOURCE 200112L
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
#include <stdlib.h>
#include <arpa/inet.h>
#include <sys/syslog.h>
#include <sys/types.h>
#include <sys/socket.h>
#include <netdb.h>
#include <signal.h>
#include <fcntl.h>
#include <assert.h>
#include <pthread.h>
#include <stdbool.h>
#include <sys/time.h>
#include <unistd.h>
#include <sys/ioctl.h>
#include "../aesd-char-driver/aesd_ioctl.h"

#define BUFFER_SIZE  (1024 * 32)

#define USE_AESD_CHAR_DEVICE

#if (defined USE_AESD_CHAR_DEVICE)
#define AESD_CHAR_DEV_PATH "/dev/aesdchar"
#else
#define AESD_CHAR_DEV_PATH "/var/tmp/aesdsocketdata"
#endif


typedef struct thread_args 
{
    int client_fd;
    bool completed;
} thread_args_t;

typedef struct node 
{
    thread_args_t thread_args;
    pthread_t thread_id;
    struct node *next;
}node_t;

static node_t *head = NULL;
static volatile sig_atomic_t stop = 0;
static pthread_mutex_t lock;
static pthread_t timer_thread_id;

static void clean_up_completed_threads(node_t **head)
{
    node_t *current = *head;
    node_t *prev = NULL;

    while (current != NULL)
    {
        if (current->thread_args.completed)
        {
            (void)pthread_join(current->thread_id, NULL);
            if (prev == NULL)
            {
                *head = current->next;
                free(current);
                current = *head;
            }
            else
            {
                prev->next = current->next;
                free(current);
                current = prev->next;
            }
        }
        else
        {
            prev = current;
            current = current->next;
        }
    }
}

static void remove_tail(node_t **head)
{
    if (*head == NULL)
    {
        return;
    }

    node_t *current = *head;
    node_t *prev = NULL;

    // only one node, which is head
    if(current->next == NULL)
    {
        free(current);
        *head = NULL;
        return;
    }

    // go to the last node
    while (current->next != NULL)
    {
        prev = current;
        current = current->next;
    }

    free(current);
    prev->next = NULL;
}

static node_t * insert_tail(node_t **head, int client_fd)
{
    node_t *new_node = malloc(sizeof(node_t));
    new_node->thread_args.client_fd = client_fd;
    new_node->thread_args.completed = false;
    // thread id will be set later
    new_node->next = NULL;

    if(*head == NULL)
    {        
        *head = new_node;
        return new_node;
    }

    node_t *current = *head;
    while(current->next != NULL)
    {
        current = current->next;
    }
    current->next = new_node;
    return new_node;
}

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        stop = 1;
    }
}

static int check_ioctl_seekto(const char *in, uint32_t* x, uint32_t* y)
{
    const char *prefix = "AESDCHAR_IOCSEEKTO:";
    size_t length = strlen(prefix);

    if(0 != strncmp(in, prefix, length))
    {
        return 0;
    }

    return sscanf(&in[length], "%u,%u", x, y) == 2;
}

static void timer_thread(void* arg)
{
    time_t t;
    time_t t_start = time(NULL);
    struct tm *tmp_time;
    char *time_buffer = malloc(100);
    struct timespec req;
    req.tv_sec  = 0;
    req.tv_nsec = 100 * 1000 * 1000;   // 100 ms = 100,000,000 ns

    while (!stop)
    {
         // Sleep for 100 ms
        nanosleep(&req, NULL);
        t = time(NULL);
        if (difftime(t, t_start) >= 10)
        {
            tmp_time = localtime(&t);

            strftime(time_buffer, 100, "timestamp: %Y-%m-%d %H:%M:%S\n", tmp_time);
            fprintf(stdout, "timestamp: %s", time_buffer);
            pthread_mutex_lock(&lock);
            int fd = open(AESD_CHAR_DEV_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
            //write(fd, time_buffer, strlen(time_buffer));
            close(fd);
            pthread_mutex_unlock(&lock);
            
            t_start = t;
        }
    }

    free(time_buffer);
}

static void* worker_thread(void* arg)
{
    thread_args_t *thread_args = (thread_args_t *)arg;

    char *buffer = malloc(BUFFER_SIZE+1); // add space for terminating 0
    ssize_t bytes_read;
    struct aesd_seekto seekto;

    int fd_clnt = thread_args->client_fd;

    pthread_mutex_lock(&lock);   // lock the mutex

    // open file
    int fd = open(AESD_CHAR_DEV_PATH, O_WRONLY | O_CREAT | O_APPEND, 0644);
    // read from socket and write to file
    while (1)
    {
        bytes_read = recv(fd_clnt, buffer, BUFFER_SIZE, 0);
        if(bytes_read > 0)
        {
            buffer[bytes_read] = '\0'; // Null-terminate for printing and str operations
            if(check_ioctl_seekto(buffer, &seekto.write_cmd, &seekto.write_cmd_offset))
            {
                fprintf(stdout, "ioctl: %s, %u,%u", buffer, seekto.write_cmd, seekto.write_cmd_offset);
                ioctl(fd, AESDCHAR_IOCSEEKTO, &seekto);
                break;
            }
            else
            {
                write(fd, buffer, bytes_read);
            }

            if(buffer[bytes_read-1] == '\n')
            {
                close(fd);
                // Send entire file back to socket
                fd = open(AESD_CHAR_DEV_PATH, O_RDONLY);
                ssize_t file_bytes;
                while ((file_bytes = read(fd, buffer, BUFFER_SIZE)) > 0)
                {
                    fprintf(stdout, "Sending %zd bytes back to client\n", file_bytes);
                    send(fd_clnt, buffer, file_bytes, 0);
                }
                break;
            }
        }
        else
        {
            break;
        }
    }
    close(fd);

    pthread_mutex_unlock(&lock); // unlock the mutex

    if(bytes_read == 0)
    {
        fprintf(stdout, "Client disconnected\n");
        syslog(LOG_INFO, "Client disconnected");
    }
    else if (bytes_read == -1)
    {
        fprintf(stderr, "recv failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "recv failed: %s", strerror(errno));
    }
    else
    {
        
    }
    
    return NULL;
}

int main(int argc, char *argv[])
{
    uint32_t fork_process = 0;

    if(argc == 2)
    {
        // verify argument is "-d"
        if(strcmp(argv[1], "-d") == 0)
        {
            fork_process = 1;
        }
    }

#if (!defined USE_AESD_CHAR_DEVICE)
    const char *filename = AESD_CHAR_DEV_PATH;
    (void)remove(filename); // Alte Datei entfernen, falls vorhanden
#endif

    openlog("aesdsocket", LOG_PID | LOG_CONS, LOG_USER);
   
    struct sigaction sa;
    memset(&sa, 0, sizeof(sa));
    sa.sa_handler = signal_handler;
    sigemptyset(&sa.sa_mask);
    sa.sa_flags = 0; // wichtig: kein SA_RESTART
    if (sigaction(SIGINT, &sa, NULL) == -1 || sigaction(SIGTERM, &sa, NULL) == -1)
    {
        fprintf(stderr, "Failed to set signal handler: %s\n", strerror(errno));
        syslog(LOG_ERR, "Failed to set signal handler: %s", strerror(errno));
        closelog();
        return -1;
    }

    fprintf(stdout, "Signal handlers set successfully\n");

    int fd_srv = socket(AF_INET, SOCK_STREAM, 0);
    if (fd_srv == -1)
    {
        fprintf(stderr, "Socket creation failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "Socket creation failed: %s", strerror(errno));
        closelog();
        return -1;
    }

    fprintf(stdout, "Socket created successfully\n");


    int optval = 1;
    if (setsockopt(fd_srv, SOL_SOCKET, SO_REUSEADDR, &optval, sizeof(optval)) < 0) 
    {
        fprintf(stderr, "setsockopt failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "setsockopt failed: %s", strerror(errno));
        close(fd_srv);
        closelog();
        return -1;        
    }


    struct addrinfo hints, *servinfo;
    memset(&hints, 0, sizeof(hints));
    hints.ai_family = AF_INET;
    hints.ai_socktype = SOCK_STREAM;
    hints.ai_flags = AI_PASSIVE;

    int ret = getaddrinfo(NULL, "9000", &hints, &servinfo);
    if (ret != 0)
    {
        fprintf(stderr, "getaddrinfo failed: %s\n", gai_strerror(ret));
        syslog(LOG_ERR, "getaddrinfo failed: %s", gai_strerror(ret));
        close(fd_srv);
        closelog();
        return -1;
    }

    fprintf(stdout, "getaddrinfo succeeded: %s\n", inet_ntoa(((struct sockaddr_in *)servinfo->ai_addr)->sin_addr));

    if (bind(fd_srv, servinfo->ai_addr, servinfo->ai_addrlen) == -1)
    {
        fprintf(stderr, "Socket bind failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "Socket bind failed: %s", strerror(errno));
        freeaddrinfo(servinfo);
        close(fd_srv);
        closelog();
        return -1;
    }
    freeaddrinfo(servinfo);

    if(fork_process == 1)
    {
        fprintf(stdout, "Forking process to background\n"); 
        // Fork the process
        pid_t pid = fork();
        if (pid < 0)
        {
            fprintf(stderr, "Fork failed: %s\n", strerror(errno));
            syslog(LOG_ERR, "Fork failed: %s", strerror(errno));
            close(fd_srv);
            closelog();
            return -1;
        }
        if (pid > 0)
        {
            // Parent process exits
            exit(0);
        }
    }

    fprintf(stdout, "Socket bound successfully\n");

    if (listen(fd_srv, 1) != 0)
    {
        fprintf(stderr, "Socket listen failed: %s\n", strerror(errno));
        syslog(LOG_ERR, "Socket listen failed: %s", strerror(errno));
        close(fd_srv);
        closelog();
        return -1;
    }

    fprintf(stdout, "Server listening on port 9000\n");
    syslog(LOG_INFO, "Server listening on port 9000");

    if (pthread_mutex_init(&lock, NULL) != 0) 
    {
        fprintf(stderr, "Mutex init failed\n");
        syslog(LOG_ERR, "Mutex init failed");
        close(fd_srv);
        closelog();
        return -1;
    }

    pthread_create(&timer_thread_id, NULL, (void *)timer_thread, NULL);

    while (!stop)
    {
        struct sockaddr_in client_addr;
        socklen_t addrlen = sizeof(client_addr);
        int fd_clnt = accept(fd_srv, (struct sockaddr *)&client_addr, &addrlen);
        if (fd_clnt == -1)
        {
            if (errno == EINTR && stop)
            {
                fprintf(stdout, "Signal received, shutting down server\n");
                break; // Signal empfangen, sauber beenden
            }
            fprintf(stderr, "accept failed: %s\n", strerror(errno));
            syslog(LOG_ERR, "accept failed: %s", strerror(errno));
            continue;
        }

        fprintf(stdout, "Accepted connection from %s\n", inet_ntoa(client_addr.sin_addr));
        syslog(LOG_INFO, "Accepted connection from %s", inet_ntoa(client_addr.sin_addr));

        // char temp_buffer[32];
        // recv(fd_clnt, temp_buffer, sizeof(temp_buffer) - 1, 0);
        // fprintf(stdout, "Peeked data: %s\n", temp_buffer);

        node_t * n = insert_tail(&head, fd_clnt);
        // fprintf(stdout, "fd_client %d inserted into thread list\n", fd_clnt);
        // Create thread
        pthread_t thread_id;
        int ret = pthread_create(&thread_id, NULL, worker_thread, &n->thread_args);
        if (ret != 0) 
        {            
            remove_tail(&head);
            fprintf(stderr, "Error creating thread: %d\n", ret);
            syslog(LOG_ERR, "Error creating thread: %d", ret);
            close(fd_clnt);
            continue;
        }
        else
        {
            fprintf(stdout, "Worker thread created successfully\n");
        }
        n->thread_id = thread_id; 
        
        // Clean up completed threads
        clean_up_completed_threads(&head);
    }

    pthread_mutex_destroy(&lock);

    (void)pthread_join(timer_thread_id, NULL);

    // Clean up remaining threads
    while (head != NULL)
    {
        remove_tail(&head);
    }

#if (!defined USE_AESD_CHAR_DEVICE)
    remove(AESD_CHAR_DEV_PATH); // Remove existing file
#endif
    fprintf(stdout, "Shutting down server\n");
    syslog(LOG_INFO, "Caught signal, exiting");
    close(fd_srv);
    closelog();
        
    return 0;
}