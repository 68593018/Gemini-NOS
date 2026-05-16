#include <string.h>
#include "nos_manifest.h"

static const nos_node_def_t g_nodes[] = {
    {
        .name = "ProcA",
        .uds_path = "/tmp/nos_proc_A.sock",
        .threads = {
            { .name = "Thread-0", .comp_ids = {1, 0} },    /* Comp-1 在线程 0 */
            { .name = "Thread-1", .comp_ids = {2, 3, 0} }, /* Comp-2, 3 在线程 1 */
            { .name = NULL }
        }
    },
    {
        .name = "ProcB",
        .uds_path = "/tmp/nos_proc_B.sock",
        .threads = {
            { .name = "Thread-0", .comp_ids = {4, 5, 0} }, /* Comp-4, 5 在线程 0 */
            { .name = NULL }
        }
    }
};

static const nos_service_def_t g_services[] = {
    { .service_id = 101, .node_name = "ProcA", .provider_comp_id = 1 },
    { .service_id = 204, .node_name = "ProcB", .provider_comp_id = 4 }
};

const nos_node_def_t* nos_manifest_get_node(const char *node_name) {
    for (size_t i = 0; i < sizeof(g_nodes)/sizeof(g_nodes[0]); i++) {
        if (strcmp(g_nodes[i].name, node_name) == 0) {
            return &g_nodes[i];
        }
    }
    return NULL;
}

const nos_service_def_t* nos_manifest_get_services(uint32_t *count) {
    *count = sizeof(g_services) / sizeof(g_services[0]);
    return g_services;
}
