CC ?= gcc
CPPFLAGS += -D_GNU_SOURCE -D_FILE_OFFSET_BITS=64
CFLAGS ?= -O2
CFLAGS += -std=c11 -Wall -Wextra -Wpedantic
COMMON = protocol.o transfer.o

.PHONY: all clean
all: server client

server: chat_server.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

client: chat_client.o $(COMMON)
	$(CC) $(LDFLAGS) -o $@ $^ $(LDLIBS)

chat_server.o chat_client.o protocol.o transfer.o: protocol.h
chat_server.o chat_client.o transfer.o: transfer.h

clean:
	$(RM) server client *.o
