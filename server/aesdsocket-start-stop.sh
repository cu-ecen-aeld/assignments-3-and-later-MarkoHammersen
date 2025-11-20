#! bin/sh


# add a startup script aesdsocket-start-stop which uses start-stop-daemon to start your aesdsocket application in daemon mode with the -d option.
case "$1" in
  start)
    echo "Starting aesdsocket..."
    start-stop-daemon -S -n aesdsocket -a /usr/bin/aesdsocket -- -d
    ;;
  stop)
    echo "Stopping aesdsocket..."
    start-stop-daemon -K -n aesdsocket
    ;;
  *)
    echo "Usage: $0 {start|stop}"
    exit 1
    ;;
esac

exit 0