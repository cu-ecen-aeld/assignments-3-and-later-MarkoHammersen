CC ?= $(CC)
CFLAGS ?= -g -Wall -Werror
LDFLAGS ?=
TARGET := aesdsocket

all: $(TARGET)

$(TARGET): aesdsocket.c
	$(CC) $(CFLAGS) $(LDFLAGS) -o $(TARGET) aesdsocket.c

clean:
	rm -f $(TARGET) *.o
