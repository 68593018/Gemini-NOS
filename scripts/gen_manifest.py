import sys
import os

try:
    import yaml
    USE_PYYAML = True
except ImportError:
    USE_PYYAML = False

def parse_yaml_fallback(file_path):
    data = {"nodes": [], "services": [], "components": []}
    if not os.path.exists(file_path): return data
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
        
    mode = None
    for line in lines:
        line = line.rstrip()
        if not line or line.lstrip().startswith('#'): continue
        indent = len(line) - len(line.lstrip())
        content = line.strip()
        
        if content == "nodes:": mode = "nodes"; continue
        if content == "services:": mode = "services"; continue
        if content == "components:": mode = "components"; continue
            
        if mode == "nodes":
            if content.startswith("- name:"):
                data["nodes"].append({"name": content.split(":")[1].strip().strip('"'), "threads": []})
            elif "uds_path:" in content:
                data["nodes"][-1]["uds_path"] = content.split(":")[1].strip().strip('"')
            elif content.startswith("- name:") and indent > 4:
                data["nodes"][-1]["threads"].append({"name": content.split(":")[1].strip().strip('"'), "components": []})
            elif "components:" in content:
                # 处理 ["Comp-1", "Comp-2"] 格式
                comps = content.split(":")[1].strip().strip('[]').split(',')
                data["nodes"][-1]["threads"][-1]["components"] = [c.strip().strip('"') for c in comps if c.strip()]
        
        elif mode == "services":
            if content.startswith("- id:"):
                data["services"].append({"id": int(content.split(":")[1].strip())})
            elif "provider:" in content:
                data["services"][-1]["provider"] = content.split(":")[1].strip().strip('"')
                
        elif mode == "components":
            if content.startswith("- name:"):
                data["components"].append({"name": content.split(":")[1].strip().strip('"')})
            elif "id:" in content:
                data["components"][-1]["id"] = int(content.split(":")[1].strip())
                
    return data

def merge_config(base, new):
    if "nodes" in new: base["nodes"].extend(new["nodes"])
    if "services" in new: base["services"].extend(new["services"])
    if "components" in new: base["components"].extend(new["components"])

def validate_and_resolve(data):
    # 1. 建立 组件名 -> ID 的映射
    name_to_id = {c["name"]: c["id"] for c in data["components"]}
    
    # 2. 解析 Node 中的组件名称
    comp_to_node = {}
    for node in data["nodes"]:
        for thread in node["threads"]:
            resolved_comp_ids = []
            for comp_name in thread["components"]:
                # 如果已经是数字（兼容旧版或强制ID），直接转换
                if isinstance(comp_name, int) or comp_name.isdigit():
                    cid = int(comp_name)
                elif comp_name in name_to_id:
                    cid = name_to_id[comp_name]
                else:
                    print(f"Error: Unknown component name '{comp_name}' in node {node['name']}")
                    sys.exit(1)
                
                resolved_comp_ids.append(cid)
                if cid in comp_to_node:
                    print(f"Error: Component ID {cid} (from {comp_name}) deployed multiple times!")
                    sys.exit(1)
                comp_to_node[cid] = node["name"]
            thread["components"] = resolved_comp_ids # 替换为 ID 列表

    # 3. 解析 Service 中的提供者名称
    for svc in data["services"]:
        provider = svc["provider"]
        if isinstance(provider, int) or str(provider).isdigit():
            pid = int(provider)
        elif provider in name_to_id:
            pid = name_to_id[provider]
        else:
            print(f"Error: Unknown provider name '{provider}' in service {svc['id']}")
            sys.exit(1)
            
        svc["provider_id"] = pid
        if pid in comp_to_node:
            svc["node"] = comp_to_node[pid]
        else:
            print(f"Error: Service {svc['id']} provider {provider} not found in deployment!")
            sys.exit(1)

    print(f"Resolution Successful: {len(data['nodes'])} Nodes, {len(data['services'])} Services (via Name mapping).")

def generate_c_code(data, output_path):
    c_content = [
        '#include <string.h>',
        '#include "nos_manifest.h"',
        '',
        'static const nos_node_def_t g_nodes[] = {'
    ]
    for node in data["nodes"]:
        c_content.append('    {')
        c_content.append(f'        .name = "{node["name"]}",')
        c_content.append(f'        .uds_path = "{node["uds_path"]}",')
        c_content.append('        .threads = {')
        for thread in node["threads"]:
            comps_str = ", ".join(map(str, thread["components"])) + ", 0"
            c_content.append(f'            {{ .name = "{thread["name"]}", .comp_ids = {{{comps_str}}} }},')
        c_content.append('            { .name = NULL }')
        c_content.append('        }')
        c_content.append('    },')
    c_content.append('};')
    c_content.append('')
    c_content.append('static const nos_service_def_t g_services[] = {')
    for svc in data["services"]:
        c_content.append(f'    {{ .service_id = {svc["id"]}, .node_name = "{svc["node"]}", .provider_comp_id = {svc["provider_id"]} }},')
    c_content.append('};')
    c_content.append('')
    c_content.append('const nos_node_def_t* nos_manifest_get_node(const char *node_name) {')
    c_content.append('    for (size_t i = 0; i < sizeof(g_nodes)/sizeof(g_nodes[0]); i++) {')
    c_content.append('        if (strcmp(g_nodes[i].name, node_name) == 0) return &g_nodes[i];')
    c_content.append('    }')
    c_content.append('    return NULL;')
    c_content.append('}')
    c_content.append('')
    c_content.append('const nos_service_def_t* nos_manifest_get_services(uint32_t *count) {')
    c_content.append('    *count = sizeof(g_services) / sizeof(g_services[0]);')
    c_content.append('    return g_services;')
    c_content.append('}')

    with open(output_path, 'w') as f:
        f.write("\n".join(c_content))
    print(f"Successfully generated {output_path} with name-to-id mapping.")

if __name__ == "__main__":
    if len(sys.argv) < 3: sys.exit(1)
    input_dir, output_path = sys.argv[1], sys.argv[2]
    master_data = {"nodes": [], "services": [], "components": []}
    for root, dirs, files in os.walk(input_dir):
        for file in files:
            if file.endswith(".yaml") or file.endswith(".yml"):
                full_path = os.path.join(root, file)
                if USE_PYYAML:
                    with open(full_path, 'r') as f: file_data = yaml.safe_load(f)
                else: file_data = parse_yaml_fallback(full_path)
                if file_data: merge_config(master_data, file_data)
    validate_and_resolve(master_data)
    generate_c_code(master_data, output_path)
