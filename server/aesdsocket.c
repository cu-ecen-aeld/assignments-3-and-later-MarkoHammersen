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

#define BUFFER_SIZE  (1024 * 32)

static volatile sig_atomic_t stop = 0;

static void signal_handler(int signum)
{
    if (signum == SIGINT || signum == SIGTERM)
    {
        stop = 1;
    }
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


    const char *filename ="/var/tmp/aesdsocketdata";
    (void)remove(filename); // Alte Datei entfernen, falls vorhanden

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

    char buffer[BUFFER_SIZE];
    ssize_t bytes_read;
    size_t i = 0;

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

        i = 0;
        while ((bytes_read = recv(fd_clnt, &buffer[i], 1, 0)) > 0)
        {
            // fprintf(stdout, "Received byte: %d\n", buffer[i]);
            if (buffer[i] == 10) // Newline character
            {
                // O_WRONLY: write only
                // O_CREAT: create if not exists
                // O_APPEND: append to end if exists
                int fd = open(filename, O_WRONLY | O_CREAT | O_APPEND, 0644);
                write(fd, buffer, i + 1);
                close(fd);
                
                // Send entire file back
                fd = open(filename, O_RDONLY);
                ssize_t file_bytes;
                while ((file_bytes = read(fd, buffer, BUFFER_SIZE)) > 0)
                {
                    fprintf(stdout, "Sending %zd bytes back to client\n", file_bytes);
                    send(fd_clnt, buffer, file_bytes, 0);
                }
                close(fd);
                i = 0;
            }
            else
            {
                i++;
                assert(i < BUFFER_SIZE);
            }
        }
        close(fd_clnt);

        if(bytes_read == 0)
        {
            fprintf(stdout, "Client disconnected\n");
            syslog(LOG_INFO, "Client disconnected");
        }
    }

    fprintf(stdout, "Shutting down server\n");
    syslog(LOG_INFO, "Caught signal, exiting");
    close(fd_srv);
    remove("/var/tmp/aesdsocketdata");
    closelog();
        
    return 0;
}