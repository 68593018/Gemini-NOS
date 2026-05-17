#include <string.h>
#include "nos_manifest.h"

static const nos_buffer_pool_def_t g_proca_pools[] = {
    { .chunk_size = 32, .chunk_count = 256 },
    { .chunk_size = 64, .chunk_count = 128 },
    { .chunk_size = 128, .chunk_count = 64 },
    { .chunk_size = 0 }
};

static const nos_buffer_pool_def_t g_procb_pools[] = {
    { .chunk_size = 2048, .chunk_count = 512 },
    { .chunk_size = 4096, .chunk_count = 256 },
    { .chunk_size = 0 }
};

static const nos_node_def_t g_nodes[] = {
    {
        .name = "ProcA", .uds_path = "/tmp/nos_proc_A.sock",
        .buffer_pools = g_proca_pools,
        .threads = {
            { .name = "Worker-1", .comp_ids = {1, 0}, .comp_names = {"Comp-1", NULL}, .comp_models = {"libcomp-1.so", NULL} },
            { .name = "Worker-2", .comp_ids = {2, 3, 5, 0}, .comp_names = {"Comp-2", "Comp-3", "Comp-5", NULL}, .comp_models = {"libcomp-2.so", "libcomp-3.so", "libcomp-2.so", NULL} },
            { .name = NULL }
        }
    },
    {
        .name = "ProcB", .uds_path = "/tmp/nos_proc_B.sock",
        .buffer_pools = g_procb_pools,
        .threads = {
            { .name = "Worker-1", .comp_ids = {4, 0}, .comp_names = {"Comp-4", NULL}, .comp_models = {"libcomp-4.so", NULL} },
            { .name = NULL }
        }
    },
};
static const nos_service_def_t g_services[] = {
    { .service_id = 101, .node_name = "ProcA", .provider_comp_id = 1 },
    { .service_id = 102, .node_name = "ProcA", .provider_comp_id = 2 },
    { .service_id = 105, .node_name = "ProcA", .provider_comp_id = 5 },
    { .service_id = 204, .node_name = "ProcB", .provider_comp_id = 4 },
};

const nos_node_def_t* nos_manifest_get_node(const char *n) {
    for (size_t i=0; i<sizeof(g_nodes)/sizeof(g_nodes[0]); i++) if(strcmp(g_nodes[i].name, n)==0) return &g_nodes[i];
    return NULL;
}

const nos_service_def_t* nos_manifest_get_services(uint32_t *c) { *c = sizeof(g_services)/sizeof(g_services[0]); return g_services; }