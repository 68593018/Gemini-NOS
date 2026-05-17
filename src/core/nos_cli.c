#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "nos_cli.h"
#include "nos_node_priv.h"
#include "nos_node_mgr.h"
#include "nos_manifest.h"

/* --- 命令处理函数声明 --- */
static void do_help(const char *args);
static void do_quit(const char *args);
static void do_show_components(const char *args);
static void do_show_services(const char *args);
static void do_load(const char *args);
static void do_unload(const char *args);

/**
 * @brief CLI 命令定义结构
 */
typedef struct {
    const char *name;
    const char *help;
    void (*handler)(const char *args);
} nos_cli_cmd_t;

/* --- 命令分发表 --- */
static nos_cli_cmd_t g_cli_cmds[] = {
    {"help",            "Show this help message",           do_help},
    {"show components", "List all loaded components",       do_show_components},
    {"show services",   "List all service routes",         do_show_services},
    {"load",            "Load/Reload a component by name",  do_load},
    {"unload",          "Unload a component by name",       do_unload},
    {"quit",            "Shutdown the node",                do_quit},
    {NULL, NULL, NULL}
};

/* --- 辅助工具函数 --- */

static const char* comp_status_str(nos_comp_status_t st) {
    switch(st) {
        case NOS_COMP_ST_LOADED:  return "Loaded";
        case NOS_COMP_ST_INITED:  return "Inited";
        case NOS_COMP_ST_ACTIVE:  return "Active";
        case NOS_COMP_ST_STOPPED: return "Stopped";
        case NOS_COMP_ST_ERROR:   return "Error";
        default: return "Unknown";
    }
}

/* --- 命令处理函数实现 --- */

static void do_help(const char *args) {
    printf("\n--- NOS Node CLI Commands ---\n");
    for (int i = 0; g_cli_cmds[i].name; i++) {
        printf("  %-18s - %s\n", g_cli_cmds[i].name, g_cli_cmds[i].help);
    }
    printf("-----------------------------\n");
}

static void do_quit(const char *args) {
    g_node_ctx.keep_running = 0;
}

static void do_show_components(const char *args) {
    printf("\n%-15s %-4s %-10s %-15s %-12s\n", "Name", "ID", "Status", "Model-Lib", "Thread");
    printf("----------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
        loaded_comp_info_t *info = &g_node_ctx.loaded_info[i];
        printf("%-15s %-4u %-10s %-15s %-12s\n", 
               info->comp->name, info->comp->id, comp_status_str(info->comp->status), 
               info->lib_name, info->owner_thread->name);
    }
}

static void do_show_services(const char *args) {
    const nos_node_def_t *node = g_node_ctx.node_def;
    printf("\n%-18s %-8s %-10s %-8s %-10s\n", "Name", "Svc-ID", "Node", "Comp-ID", "Location");
    printf("----------------------------------------------------------------------\n");
    for (uint32_t i = 0; i < node->service_count; i++) {
        const nos_service_def_t *svc = &node->services[i];
        const char *location = "Remote";
        if (strcmp(svc->node_name, "Platform") == 0) {
            location = "Embedded";
        } else if (strcmp(svc->node_name, node->name) == 0) {
            location = "Local";
        }
        printf("%-18s %-8u %-10s %-8u %-10s\n", 
               svc->service_name, svc->service_id, svc->node_name, 
               svc->provider_comp_id, location);
    }
}

static void do_load(const char *args) {
    if (!args || strlen(args) == 0) {
        printf("Usage: load <comp_name>\n");
        return;
    }
    node_reload_component(args);
}

static void do_unload(const char *args) {
    if (!args || strlen(args) == 0) {
        printf("Usage: unload <comp_name>\n");
        return;
    }
    node_unload_component(args);
}

/* --- CLI 交互核心逻辑 (Tab 补全与循环) --- */

static int advanced_get_line(char *buf, int size, const char *prompt) {
    struct termios oldt, newt;
    int pos = 0;
    char c;

    printf("%s", prompt);
    fflush(stdout);

    tcgetattr(STDIN_FILENO, &oldt);
    newt = oldt;
    newt.c_lflag &= ~(ICANON | ECHO);
    tcsetattr(STDIN_FILENO, TCSANOW, &newt);

    while (pos < size - 1) {
        if (read(STDIN_FILENO, &c, 1) <= 0) { pos = -1; break; }

        if (c == '\n' || c == '\r') {
            buf[pos] = '\0'; printf("\n"); break;
        } else if (c == 127 || c == 8) { /* Backspace */
            if (pos > 0) { pos--; printf("\b \b"); fflush(stdout); }
        } else if (c == '\t') { /* Tab 补全逻辑 */
            buf[pos] = '\0';
            int matches = 0, last_idx = -1;
            for (int i = 0; g_cli_cmds[i].name; i++) {
                if (strncmp(g_cli_cmds[i].name, buf, pos) == 0) {
                    matches++; last_idx = i;
                }
            }
            if (matches == 1) {
                printf("%s", g_cli_cmds[last_idx].name + pos);
                strcpy(buf + pos, g_cli_cmds[last_idx].name + pos);
                pos = strlen(buf);
            } else if (matches > 1) {
                printf("\n");
                for (int i = 0; g_cli_cmds[i].name; i++) {
                    if (strncmp(g_cli_cmds[i].name, buf, pos) == 0) printf("%s  ", g_cli_cmds[i].name);
                }
                printf("\n%s%s", prompt, buf);
            }
            fflush(stdout);
        } else {
            buf[pos++] = c; printf("%c", c); fflush(stdout);
        }
    }
    tcsetattr(STDIN_FILENO, TCSANOW, &oldt);
    return pos;
}

static void* node_cli_thread_entry(void *arg) {
    char cmd_buf[256];
    char prompt[64];
    int is_tty = isatty(STDIN_FILENO);
    sprintf(prompt, "nos-node(%s)> ", (char*)arg);

    if (is_tty) printf("[CLI] Management interface started. (Tab for completion)\n");

    while (g_node_ctx.keep_running) {
        int len;
        if (is_tty) {
            len = advanced_get_line(cmd_buf, sizeof(cmd_buf), prompt);
        } else {
            if (!fgets(cmd_buf, sizeof(cmd_buf), stdin)) break;
            cmd_buf[strcspn(cmd_buf, "\n")] = 0;
            len = strlen(cmd_buf);
        }

        if (len < 0) break;
        if (len == 0) continue;

        /* 命令分发逻辑 */
        int found = 0;
        for (int i = 0; g_cli_cmds[i].name; i++) {
            int name_len = strlen(g_cli_cmds[i].name);
            /* 匹配完整命令名或以命令名开头的带参数指令 */
            if (strcmp(cmd_buf, g_cli_cmds[i].name) == 0 || 
               (strncmp(cmd_buf, g_cli_cmds[i].name, name_len) == 0 && cmd_buf[name_len] == ' ')) {
                
                const char *args = (cmd_buf[name_len] == ' ') ? (cmd_buf + name_len + 1) : NULL;
                g_cli_cmds[i].handler(args);
                found = 1;
                break;
            }
        }

        if (!found && is_tty) {
            printf("Unknown command: %s. Type 'help' for support.\n", cmd_buf);
        }
    }
    return NULL;
}

void node_cli_start(const char *node_name) {
    pthread_create(&g_node_ctx.cli_tid, NULL, node_cli_thread_entry, (void*)node_name);
}
