import sys
import os

# 尝试导入 yaml，如果不存在则使用极简自定义解析
try:
    import yaml
    USE_PYYAML = True
except ImportError:
    USE_PYYAML = False

def parse_yaml_fallback(file_path):
    # 极简解析逻辑：仅处理本项目特定的 deploy.yaml 缩进和结构
    # 作为一个工程化底座，推荐环境安装 PyYAML，此处为容错实现
    data = {"nodes": [], "services": []}
    current_node = None
    current_thread = None
    
    with open(file_path, 'r') as f:
        lines = f.readlines()
        
    mode = None
    for line in lines:
        line = line.rstrip()
        if not line or line.startswith('#'): continue
        
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
            elif content.startswith("- name:") and indent > 4: # 线程级 name
                current_thread = {"name": content.split(":")[1].strip().strip('"'), "components": []}
                current_node["threads"].append(current_thread)
            elif "components:" in content:
                comps = content.split(":")[1].strip().strip('[]').split(',')
                current_thread["components"] = [int(c.strip()) for c in comps if c.strip()]
        
        elif mode == "services":
            if content.startswith("- id:"):
                data["services"].append({"id": int(content.split(":")[1].strip())})
            elif "node:" in content:
                data["services"][-1]["node"] = content.split(":")[1].strip().strip('"')
            elif "provider:" in content:
                data["services"][-1]["provider"] = int(content.split(":")[1].strip())
                
    return data

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
    print(f"Successfully generated {output_path} from YAML.")

if __name__ == "__main__":
    if len(sys.argv) < 3:
        print("Usage: python3 gen_manifest.py <input.yaml> <output.c>")
        sys.exit(1)
        
    yaml_path = sys.argv[1]
    output_path = sys.argv[2]
    
    if USE_PYYAML:
        with open(yaml_path, 'r') as f:
            data = yaml.safe_load(f)
    else:
        print("Warning: PyYAML not found, using fallback parser.")
        data = parse_yaml_fallback(yaml_path)
        
    generate_c_code(data, output_path)
