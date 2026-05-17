#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <termios.h>
#include <unistd.h>
#include "nos_cli.h"
#include "nos_node_priv.h"
#include "nos_node_mgr.h"
#include "nos_manifest.h"

static char* cli_commands[] = {
    "show components",
    "show services",
    "load",
    "unload",
    "help",
    "quit",
    NULL
};

static void cli_print_help(void) {
    printf("\n--- NOS Node CLI Commands ---\n");
    printf("  show components       - List all loaded components\n");
    printf("  show services         - List all service routes\n");
    printf("  load <comp_name>      - Reload/Load a component\n");
    printf("  unload <comp_name>    - Unload a component\n");
    printf("  help                  - Show this help\n");
    printf("  quit                  - Shutdown the node\n");
    printf("-----------------------------\n");
}

/* 高级模式：支持 Tab 补全的行读取 */
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
        if (read(STDIN_FILENO, &c, 1) <= 0) {
            pos = -1; break;
        }

        if (c == '\n' || c == '\r') {
            buf[pos] = '\0';
            printf("\n");
            break;
        } else if (c == 127 || c == 8) { /* Backspace */
            if (pos > 0) {
                pos--;
                printf("\b \b");
                fflush(stdout);
            }
        } else if (c == '\t') { /* Tab 补全逻辑 */
            buf[pos] = '\0';
            int matches = 0;
            int last_idx = -1;
            for (int i = 0; cli_commands[i]; i++) {
                if (strncmp(cli_commands[i], buf, pos) == 0) {
                    matches++; last_idx = i;
                }
            }
            if (matches == 1) {
                printf("%s", cli_commands[last_idx] + pos);
                strcpy(buf + pos, cli_commands[last_idx] + pos);
                pos = strlen(buf);
            } else if (matches > 1) {
                printf("\n");
                for (int i = 0; cli_commands[i]; i++) {
                    if (strncmp(cli_commands[i], buf, pos) == 0) printf("%s  ", cli_commands[i]);
                }
                printf("\n%s%s", prompt, buf);
            }
            fflush(stdout);
        } else {
            buf[pos++] = c;
            printf("%c", c);
            fflush(stdout);
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
            /* 管道输入模式：使用普通 fgets */
            if (!fgets(cmd_buf, sizeof(cmd_buf), stdin)) break;
            cmd_buf[strcspn(cmd_buf, "\n")] = 0;
            len = strlen(cmd_buf);
        }

        if (len < 0) break;
        if (len == 0) continue;

        if (strcmp(cmd_buf, "help") == 0) cli_print_help();
        else if (strcmp(cmd_buf, "quit") == 0) g_node_ctx.keep_running = 0;
        else if (strcmp(cmd_buf, "show components") == 0) {
            printf("\n%-15s %-4s %-15s %-12s\n", "Name", "ID", "Model-Lib", "Thread");
            printf("----------------------------------------------------------\n");
            for (uint32_t i = 0; i < g_node_ctx.loaded_count; i++) {
                loaded_comp_info_t *info = &g_node_ctx.loaded_info[i];
                printf("%-15s %-4u %-15s %-12s\n", info->comp->name, info->comp->id, info->lib_name, info->owner_thread->name);
            }
        } else if (strcmp(cmd_buf, "show services") == 0) {
            uint32_t count = 0;
            const nos_service_def_t *svc_list = nos_manifest_get_services(&count);
            printf("\n%-8s %-15s %-10s\n", "Svc-ID", "Node", "Provider-ID");
            printf("------------------------------------------\n");
            for (uint32_t i = 0; i < count; i++) printf("%-8u %-15s %-10u\n", svc_list[i].service_id, svc_list[i].node_name, svc_list[i].provider_comp_id);
        } else if (strncmp(cmd_buf, "unload ", 7) == 0) node_unload_component(cmd_buf + 7);
        else if (strncmp(cmd_buf, "load ", 5) == 0) node_reload_component(cmd_buf + 5);
        else if (!is_tty) ; // 管道模式不报错未知命令，避免污染日志
        else printf("Unknown command: %s\n", cmd_buf);
    }
    return NULL;
}

void node_cli_start(const char *node_name) {
    pthread_create(&g_node_ctx.cli_tid, NULL, node_cli_thread_entry, (void*)node_name);
}
