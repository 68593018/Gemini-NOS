CC = gcc
CFLAGS = -Iinclude -Wall
LDFLAGS = -lpthread
PYTHON = python3
GEN_SCRIPT = scripts/gen_manifest.py

SRCS = src/core/nos_scheduler.c \
       src/core/nos_manifest.c \
       src/core/nos_buffer.c \
       src/core/example_components.c \
       src/infra/nos_ipc_p2p.c \
       src/core/nos_node_main.c

TARGET = nos_node

all: src/core/nos_manifest.c $(TARGET)

src/core/nos_manifest.c: deploy.yaml $(GEN_SCRIPT)
	@echo "Generating manifest source from YAML..."
	$(PYTHON) $(GEN_SCRIPT) deploy.yaml src/core/nos_manifest.c

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

clean:
	rm -f $(TARGET) src/core/nos_manifest.c procA.log procB.log
