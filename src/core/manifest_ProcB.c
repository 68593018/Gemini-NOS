#include <string.h>
#include "nos_manifest.h"

extern void nos_log_init(void);

static const nos_buffer_pool_def_t g_local_pools[] = {
    { .chunk_size = 2048, .chunk_count = 512 },
    { .chunk_size = 4096, .chunk_count = 256 },
    { .chunk_size = 0 }
};
static const nos_service_def_t g_local_services[] = {
    { .service_name = "SVC_DATA_PROC", .service_id = 204, .node_name = "ProcB", .provider_comp_id = 4, .remote_uds_path = "/tmp/nos_proc_B.sock" },
    { .service_name = "SVC_LOG", .service_id = 1, .node_name = "Platform", .provider_comp_id = 0, .remote_uds_path = "" },
    { .service_name = NULL, .service_id = 0 }
};
static const nos_platform_init_func_t g_infra_inits[] = { nos_log_init , NULL };
const nos_node_def_t g_local_node_def = {
    .name = "ProcB", .uds_path = "/tmp/nos_proc_B.sock", .buffer_pools = g_local_pools,
    .threads = {
        { .name = "Worker-1", .comp_ids = {4, 0}, .comp_names = {"Comp-4", NULL}, .comp_models = {"libcomp-4.so", NULL} },
        { .name = NULL }
    },
    .services = g_local_services, .service_count = 2, .platform_inits = g_infra_inits
};

const nos_node_def_t* nos_manifest_get_local(void) { return &g_local_node_def; }