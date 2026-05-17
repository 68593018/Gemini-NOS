import sys
import os
import re

try:
    import yaml
    USE_PYYAML = True
except ImportError:
    USE_PYYAML = False

def to_c_macro(name):
    # 将名称转换为合法的 C 宏名 (大写, 非字母数字转下划线)
    return re.sub(r'[^a-zA-Z0-9]', '_', name).upper()

def parse_yaml_fallback(file_path):
    data = {"nodes": [], "services": [], "components": [], "profiles": [], "models": []}
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
        if content == "profiles:": mode = "profiles"; continue
        if content == "models:": mode = "models"; continue
        
        if mode == "nodes":
            if content.startswith("- name:"):
                data["nodes"].append({"name": content.split(":")[1].strip().strip('"'), "threads": [], "buffer_profile": "default"})
            elif "uds_path:" in content:
                data["nodes"][-1]["uds_path"] = content.split(":")[1].strip().strip('"')
            elif "buffer_profile:" in content:
                data["nodes"][-1]["buffer_profile"] = content.split(":")[1].strip().strip('"')
            elif content.startswith("- name:") and indent > 4:
                data["nodes"][-1]["threads"].append({"name": content.split(":")[1].strip().strip('"'), "components": []})
            elif "components:" in content:
                comps = content.split(":")[1].strip().strip('[]').split(',')
                data["nodes"][-1]["threads"][-1]["components"] = [c.strip().strip('"') for c in comps if c.strip()]
        elif mode == "components":
            if content.startswith("- name:"):
                data["components"].append({"name": content.split(":")[1].strip().strip('"'), "model": "", "services": []})
            elif "id:" in content and indent == 4:
                data["components"][-1]["id"] = int(content.split(":")[1].strip())
            elif "model:" in content:
                data["components"][-1]["model"] = content.split(":")[1].strip().strip('"')
            elif "name:" in content and indent > 4: # 解析实例下的服务
                svc_name = content.split(":")[1].strip().strip('"')
                data["components"][-1]["services"].append({"name": svc_name})
            elif "id:" in content and indent > 4:
                data["components"][-1]["services"][-1]["id"] = int(content.split(":")[1].strip())
        elif mode == "models":
            if content.startswith("- name:"):
                data["models"].append({"name": content.split(":")[1].strip().strip('"'), "lib": ""})
            elif "lib:" in content:
                data["models"][-1]["lib"] = content.split(":")[1].strip().strip('"')
        elif mode == "profiles":
            if content.startswith("- name:"):
                data["profiles"].append({"name": content.split(":")[1].strip().strip('"'), "bins": []})
            elif "size:" in content:
                parts = content.strip().strip('{}').split(',')
                bin_data = {}
                for p in parts:
                    k, v = p.split(':')
                    bin_data[k.strip()] = int(v.strip())
                data["profiles"][-1]["bins"].append(bin_data)
    return data

def validate_and_resolve(data):
    name_to_id = {c["name"]: c["id"] for c in data["components"]}
    name_to_model = {c["name"]: c["model"] for c in data["components"]}
    model_to_lib = {m["name"]: m["lib"] for m in data["models"]}
    profile_map = {p["name"]: p["bins"] for p in data["profiles"]}
    comp_to_node = {}
    
    # 提取所有服务定义
    all_services = []
    svc_name_to_id = {}
    for comp in data["components"]:
        for svc in comp.get("services", []):
            all_services.append({
                "id": svc["id"],
                "node": None, # 后面填充
                "provider_id": comp["id"]
            })
            svc_name_to_id[svc["name"]] = svc["id"]
    data["resolved_services"] = all_services
    data["svc_name_to_id"] = svc_name_to_id

    for node in data["nodes"]:
        pname = node.get("buffer_profile", "default")
        node["resolved_bins"] = profile_map.get(pname, profile_map.get("default", []))
        
        for thread in node["threads"]:
            resolved_ids = []
            resolved_names = []
            resolved_libs = []
            for comp_name in thread["components"]:
                cid = name_to_id.get(comp_name)
                mname = name_to_model.get(comp_name)
                lib = model_to_lib.get(mname)
                
                if cid is None:
                    print(f"Error: Unknown component '{comp_name}'"); sys.exit(1)
                if lib is None:
                    print(f"Error: No library defined for model '{mname}' (Comp: {comp_name})"); sys.exit(1)
                    
                resolved_ids.append(cid)
                resolved_names.append(comp_name)
                resolved_libs.append(lib)
                comp_to_node[cid] = node["name"]
                
            thread["comp_ids"] = resolved_ids
            thread["comp_names"] = resolved_names
            thread["comp_libs"] = resolved_libs
            
    # 填充服务的 Node 信息
    for svc in data["resolved_services"]:
        svc["node"] = comp_to_node.get(svc["provider_id"])
        
    return name_to_id, svc_name_to_id

def generate_header(comp_ids, svc_ids, header_path):
    lines = ["#ifndef __NOS_IDS_H__", "#define __NOS_IDS_H__", ""]
    lines.append("/* Component IDs */")
    for name, cid in sorted(comp_ids.items(), key=lambda x: x[1]):
        lines.append(f"#define {to_c_macro(name):20} {cid}")
    lines.append("")
    lines.append("/* Service IDs */")
    for name, sid in sorted(svc_ids.items(), key=lambda x: x[1]):
        lines.append(f"#define {to_c_macro(name):20} {sid}")
    lines.append("")
    lines.append("#endif")
    with open(header_path, 'w') as f: f.write("\n".join(lines))
    print(f"Generated {header_path} with {len(comp_ids)} components and {len(svc_ids)} services.")

def generate_c_code(data, output_path):
    c_content = ['#include <string.h>', '#include "nos_manifest.h"', '']
    
    # 先生成各个节点的池定义
    for node in data["nodes"]:
        safe_name = node["name"].lower()
        c_content.append(f'static const nos_buffer_pool_def_t g_{safe_name}_pools[] = {{')
        for b in node["resolved_bins"]:
            c_content.append(f'    {{ .chunk_size = {b["size"]}, .chunk_count = {b["count"]} }},')
        c_content.append('    { .chunk_size = 0 }')
        c_content.append('};\n')

    c_content.append('static const nos_node_def_t g_nodes[] = {')
    for node in data["nodes"]:
        safe_name = node["name"].lower()
        c_content.append('    {')
        c_content.append(f'        .name = "{node["name"]}", .uds_path = "{node["uds_path"]}",')
        c_content.append(f'        .buffer_pools = g_{safe_name}_pools,')
        c_content.append('        .threads = {')
        for thread in node["threads"]:
            ids_str = ", ".join(map(str, thread["comp_ids"]))
            names_str = ", ".join(f'"{n}"' for n in thread["comp_names"])
            libs_str = ", ".join(f'"{l}"' for l in thread["comp_libs"])
            c_content.append(f'            {{ .name = "{thread["name"]}", .comp_ids = {{{ids_str}, 0}}, .comp_names = {{{names_str}, NULL}}, .comp_models = {{{libs_str}, NULL}} }},')
        c_content.append('            { .name = NULL }')
        c_content.append('        }')
        c_content.append('    },')
    c_content.append('};')
    c_content.append('static const nos_service_def_t g_services[] = {')
    for svc in data["resolved_services"]:
        c_content.append(f'    {{ .service_id = {svc["id"]}, .node_name = "{svc["node"]}", .provider_comp_id = {svc["provider_id"]} }},')
    c_content.append('};\n')
    c_content.append('const nos_node_def_t* nos_manifest_get_node(const char *n) {')
    c_content.append('    for (size_t i=0; i<sizeof(g_nodes)/sizeof(g_nodes[0]); i++) if(strcmp(g_nodes[i].name, n)==0) return &g_nodes[i];')
    c_content.append('    return NULL;\n}\n')
    c_content.append('const nos_service_def_t* nos_manifest_get_services(uint32_t *c) { *c = sizeof(g_services)/sizeof(g_services[0]); return g_services; }')
    with open(output_path, 'w') as f: f.write("\n".join(c_content))

if __name__ == "__main__":
    if len(sys.argv) < 4:
        print("Usage: python3 gen_manifest.py <input_dir> <output.c> <output.h>")
        sys.exit(1)
    input_dir, out_c, out_h = sys.argv[1], sys.argv[2], sys.argv[3]
    master = {"nodes": [], "services": [], "components": [], "profiles": [], "models": []}
    for r, ds, files in os.walk(input_dir):
        for f in files:
            if f.endswith(".yaml"):
                if USE_PYYAML:
                    with open(os.path.join(r,f)) as y: fd = yaml.safe_load(y)
                else: fd = parse_yaml_fallback(os.path.join(r,f))
                if fd:
                    for k in master: master[k].extend(fd.get(k, []))
    comp_ids, svc_ids = validate_and_resolve(master)
    generate_header(comp_ids, svc_ids, out_h)
    generate_c_code(master, out_c)
