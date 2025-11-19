#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <syslog.h>
#include <errno.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/types.h>
#include <sys/stat.h>

#define MAX_STR_LEN 1024

static void log_error(const char *message) 
{
    syslog(LOG_ERR, "%s: %s", message, strerror(errno));
    closelog();
}

int main(int argc, char *argv[]) 
{
    if (argc != 3) 
    {
        fprintf(stderr, "Usage: %s <writefile> <writestr>\n", argv[0]);
        return 1;
    }

    const char *writefile = argv[1];
    const char *writestr = argv[2];

    openlog("writer_app", LOG_PID | LOG_CONS, LOG_USER);

    syslog(LOG_DEBUG, "Writing %s to %s", writestr, writefile);

    int fd = open(writefile, O_WRONLY | O_CREAT | O_TRUNC, S_IRUSR | S_IWUSR);
    if (0 > fd) 
    {
        log_error("Failed to open file for writing");
        return 1;
    }

    ssize_t bytes_written = write(fd, writestr, strlen(writestr));
    if (bytes_written != strlen(writestr)) 
    {
        close(fd);
        log_error("Failed to write to file");
    }

    if (close(fd) != 0) 
    {
        log_error("Failed to close file");
        return 1;
    }

    closelog();
    return 0;
}