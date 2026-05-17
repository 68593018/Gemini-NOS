CC = gcc
CFLAGS = -Iinclude -Wall -fPIC
LDFLAGS = -lpthread -ldl -rdynamic
PYTHON = python3
GEN_SCRIPT = scripts/gen_manifest.py
CONFIG_DIR = conf

# 核心通用源码
CORE_SRCS = src/core/nos_scheduler.c \
            src/core/nos_buffer.c \
            src/core/nos_service.c \
            src/core/nos_node_mgr.c \
            src/core/nos_cli.c \
            src/infra/nos_ipc_p2p.c \
            src/infra/nos_log.c \
            src/core/nos_node_main.c

# 组件定义
COMP_LIBS = libcomp-1.so libcomp-2.so libcomp-3.so libcomp-4.so libcomp-5.so

# 默认目标
all: include/nos_ids.h $(COMP_LIBS) nos_ProcA nos_ProcB

# 生成全局 ID 头文件
include/nos_ids.h: $(shell find $(CONFIG_DIR) -name "*.yaml") $(GEN_SCRIPT)
	@echo "Generating manifests and ID headers..."
	@$(PYTHON) $(GEN_SCRIPT) $(CONFIG_DIR) include/nos_ids.h

# 编译各节点二进制
nos_ProcA: $(CORE_SRCS) src/core/manifest_ProcA.c include/nos_ids.h
	@echo "Building binary for node ProcA..."
	@$(CC) $(CFLAGS) $(CORE_SRCS) src/core/manifest_ProcA.c -o $@ $(LDFLAGS)

nos_ProcB: $(CORE_SRCS) src/core/manifest_ProcB.c include/nos_ids.h
	@echo "Building binary for node ProcB..."
	@$(CC) $(CFLAGS) $(CORE_SRCS) src/core/manifest_ProcB.c -o $@ $(LDFLAGS)

# 组件编译规则
libcomp-1.so: src/components/model_1/model_1.c
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-2.so: src/components/model_2/model_2.c
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-3.so: src/components/model_3/model_3.c
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-4.so: src/components/model_4/model_4.c
	@$(CC) $(CFLAGS) -shared $< -o $@
libcomp-5.so: src/components/model_5/model_5.c
	@$(CC) $(CFLAGS) -shared $< -o $@

clean:
	rm -f nos_Proc* libcomp-*.so include/nos_ids.h src/core/manifest_*.c
