CC = gcc
CFLAGS = -Iinclude -Wall -fPIC
LDFLAGS = -lpthread -ldl -rdynamic
PYTHON = python3
GEN_SCRIPT = scripts/gen_manifest.py
CONFIG_DIR = conf

# 核心通用源码
CORE_SRCS = src/core/nos_scheduler.c \
            src/core/nos_buffer.c \
            src/core/nos_node_mgr.c \
            src/core/nos_cli.c \
            src/infra/nos_ipc_p2p.c \
            src/core/nos_node_main.c

# 组件源码与目标库 (保持不变)
COMP_SRCS = $(wildcard src/components/comp_*.c)
COMP_LIBS = $(patsubst src/components/comp_%.c, libcomp-%.so, $(COMP_SRCS))

# 默认目标
all: include/nos_ids.h $(COMP_LIBS) nos_ProcA nos_ProcB

# 生成全局 ID 头文件并打印出节点列表
include/nos_ids.h: $(shell find $(CONFIG_DIR) -name "*.yaml") $(GEN_SCRIPT)
	@echo "Generating manifests and ID headers..."
	@$(PYTHON) $(GEN_SCRIPT) $(CONFIG_DIR) include/nos_ids.h

# 编译 ProcA 二进制 (链接特定的 manifest_ProcA.c)
nos_ProcA: $(CORE_SRCS) src/core/manifest_ProcA.c include/nos_ids.h
	@echo "Building binary for node ProcA..."
	@$(CC) $(CFLAGS) $(CORE_SRCS) src/core/manifest_ProcA.c -o $@ $(LDFLAGS)

# 编译 ProcB 二进制 (链接特定的 manifest_ProcB.c)
nos_ProcB: $(CORE_SRCS) src/core/manifest_ProcB.c include/nos_ids.h
	@echo "Building binary for node ProcB..."
	@$(CC) $(CFLAGS) $(CORE_SRCS) src/core/manifest_ProcB.c -o $@ $(LDFLAGS)

# 组件编译规则
libcomp-%.so: src/components/comp_%.c
	@echo "Building component library $@..."
	@$(CC) $(CFLAGS) -shared $< -o $@

clean:
	rm -f nos_Proc* libcomp-*.so include/nos_ids.h src/core/manifest_*.c
