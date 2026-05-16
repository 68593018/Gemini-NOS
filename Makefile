CC = gcc
CFLAGS = -Iinclude -Wall
LDFLAGS = -lpthread

SRCS = src/core/nos_scheduler.c \
       src/core/nos_manifest.c \
       src/core/nos_buffer.c \
       src/core/example_components.c \
       src/infra/nos_ipc_p2p.c \
       src/core/nos_node_main.c

TARGET = nos_node

all: $(TARGET)

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) procA procB nos_epoll_demo nos_demo nos_async_demo
