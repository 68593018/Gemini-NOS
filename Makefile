CC = gcc
CFLAGS = -Iinclude -Wall -fPIC
LDFLAGS = -lpthread -ldl -rdynamic
PYTHON = python3
GEN_SCRIPT = scripts/gen_manifest.py
CONFIG_DIR = conf
CONFIG_FILES = $(shell find $(CONFIG_DIR) -name "*.yaml" -o -name "*.yml")

# 主程序源码
SRCS = src/core/nos_scheduler.c \
       src/core/nos_manifest.c \
       src/core/nos_buffer.c \
       src/infra/nos_ipc_p2p.c \
       src/core/nos_node_main.c

# 组件源码与目标库
COMP_SRCS = $(wildcard src/components/comp_*.c)
COMP_LIBS = $(patsubst src/components/comp_%.c, libcomp-%.so, $(COMP_SRCS))

TARGET = nos_node

all: src/core/nos_manifest.c $(TARGET) $(COMP_LIBS)

src/core/nos_manifest.c: $(CONFIG_FILES) $(GEN_SCRIPT)
	@echo "Generating manifest and ID headers from config..."
	$(PYTHON) $(GEN_SCRIPT) $(CONFIG_DIR) src/core/nos_manifest.c include/nos_ids.h

$(TARGET): $(SRCS)
	$(CC) $(CFLAGS) $(SRCS) -o $(TARGET) $(LDFLAGS)

# 组件编译规则
libcomp-%.so: src/components/comp_%.c
	@echo "Building component library $@..."
	$(CC) $(CFLAGS) -shared $< -o $@

clean:
	rm -f $(TARGET) $(COMP_LIBS) src/core/nos_manifest.c include/nos_ids.h procA.log procB.log
