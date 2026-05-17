#include <string.h>
#include "nos_manifest.h"

extern void nos_log_init(void);

static const nos_buffer_pool_def_t g_local_pools[] = {
    { .chunk_size = 32, .chunk_count = 256 },
    { .chunk_size = 64, .chunk_count = 128 },
    { .chunk_size = 128, .chunk_count = 64 },
    { .chunk_size = 0 }
};
static const nos_service_def_t g_local_services[] = {
    { .service_name = "SVC_LOG", .service_id = 1, .node_name = "Platform", .provider_comp_id = 0, .remote_uds_path = "" },
    { .service_name = "SVC_ROUTING_V4", .service_id = 102, .node_name = "ProcA", .provider_comp_id = 2, .remote_uds_path = "/tmp/nos_proc_A.sock" },
    { .service_name = "SVC_MGMT", .service_id = 101, .node_name = "ProcA", .provider_comp_id = 1, .remote_uds_path = "/tmp/nos_proc_A.sock" },
    { .service_name = "SVC_ROUTING_V6", .service_id = 105, .node_name = "ProcA", .provider_comp_id = 5, .remote_uds_path = "/tmp/nos_proc_A.sock" },
    { .service_name = "SVC_DATA_PROC", .service_id = 204, .node_name = "ProcB", .provider_comp_id = 4, .remote_uds_path = "/tmp/nos_proc_B.sock" },
    { .service_name = NULL, .service_id = 0 }
};
static const nos_platform_init_func_t g_infra_inits[] = { nos_log_init , NULL };
const nos_node_def_t g_local_node_def = {
    .name = "ProcA", .uds_path = "/tmp/nos_proc_A.sock", .buffer_pools = g_local_pools,
    .threads = {
        { .name = "Worker-1", .comp_ids = {1, 0}, .comp_names = {"Comp-1", NULL}, .comp_models = {"libcomp-1.so", NULL} },
        { .name = "Worker-2", .comp_ids = {2, 3, 5, 0}, .comp_names = {"Comp-2", "Comp-3", "Comp-5", NULL}, .comp_models = {"libcomp-2.so", "libcomp-3.so", "libcomp-2.so", NULL} },
        { .name = NULL }
    },
    .services = g_local_services, .service_count = 5, .platform_inits = g_infra_inits
};

const nos_node_def_t* nos_manifest_get_local(void) { return &g_local_node_def; }