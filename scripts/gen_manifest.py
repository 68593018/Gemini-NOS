import sys
import os

try:
    import yaml
    USE_PYYAML = True
except ImportError:
    USE_PYYAML = False

def parse_yaml_fallback(file_path):
    data = {"nodes": [], "services": []}
    current_node = None
    current_thread = None
    
    if not os.path.exists(file_path): return data
    with open(file_path, 'r') as f:
        lines = f.readlines()
        
    mode = None
    for line in lines:
        line = line.rstrip()
        if not line or line.lstrip().startswith('#'): continue
        indent = len(line) - len(line.lstrip())
        content = line.strip()
        
        if content == "nodes:":
            mode = "nodes"
            continue
        if content == "services:":
            mode = "services"
            continue
            
        if mode == "nodes":
            if content.startswith("- name:"):
                current_node = {"name": content.split(":")[1].strip().strip('"'), "uds_path": "", "threads": []}
                data["nodes"].append(current_node)
            elif "uds_path:" in content:
                current_node["uds_path"] = content.split(":")[1].strip().strip('"')
            elif content == "threads:":
                pass
            elif content.startswith("- name:") and indent > 4:
                current_thread = {"name": content.split(":")[1].strip().strip('"'), "components": []}
                current_node["threads"].append(current_thread)
            elif "components:" in content:
                comps = content.split(":")[1].strip().strip('[]').split(',')
                current_thread["components"] = [int(c.strip()) for c in comps if c.strip()]
        
        elif mode == "services":
            if content.startswith("- id:"):
                data["services"].append({"id": int(content.split(":")[1].strip())})
            elif "provider:" in content:
                data["services"][-1]["provider"] = int(content.split(":")[1].strip())
                
    return data

def merge_config(base, new):
    if "nodes" in new and new["nodes"]:
        base["nodes"].extend(new["nodes"])
    if "services" in new and new["services"]:
        base["services"].extend(new["services"])

def validate_and_resolve(data):
    # 1. 检查 Node 名唯一性
    node_names = [n["name"] for n in data["nodes"]]
    if len(node_names) != len(set(node_names)):
        print("Error: Duplicate Node names found!")
        sys.exit(1)
        
    # 2. 建立 组件 ID -> 节点名 的映射
    comp_to_node = {}
    for node in data["nodes"]:
        for thread in node["threads"]:
            for comp_id in thread["components"]:
                if comp_id in comp_to_node:
                    print(f"Error: Component {comp_id} is deployed to multiple nodes!")
                    sys.exit(1)
                comp_to_node[comp_id] = node["name"]
    
    # 3. 自动推导服务所在的 Node
    for svc in data["services"]:
        provider_id = svc.get("provider")
        if provider_id in comp_to_node:
            svc["node"] = comp_to_node[provider_id]
        else:
            print(f"Error: Service {svc['id']} provider component {provider_id} not found in any Node deployment!")
            sys.exit(1)

    print(f"Resolution Successful: {len(data['nodes'])} Nodes, {len(data['services'])} Services resolved.")

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
        c_content.append(f'    {{ .service_id = {svc["id"]}, .node_name = "{svc["node"]}", .provider_comp_id = {svc["provider"]} }},')
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
    print(f"Successfully generated {output_path} from config directory.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 gen_manifest.py <input_dir> <output.c>")
        sys.exit(1)
    input_dir = sys.argv[1]
    output_path = sys.argv[2]
    master_data = {"nodes": [], "services": []}
    for root, dirs, files in os.walk(input_dir):
        for file in files:
            if file.endswith(".yaml") or file.endswith(".yml"):
                full_path = os.path.join(root, file)
                if USE_PYYAML:
                    with open(full_path, 'r') as f:
                        file_data = yaml.safe_load(f)
                else:
                    file_data = parse_yaml_fallback(full_path)
                if file_data:
                    merge_config(master_data, file_data)
    validate_and_resolve(master_data)
    generate_c_code(master_data, output_path)
