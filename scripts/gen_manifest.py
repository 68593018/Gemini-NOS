import sys
import os
import re

try:
    import yaml
    USE_PYYAML = True
except ImportError:
    USE_PYYAML = False

def to_c_macro(name):
    return re.sub(r'[^a-zA-Z0-9]', '_', name).upper()

def parse_yaml_fallback(file_path):
    data = {"nodes": [], "components": [], "profiles": [], "models": []}
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
                data["components"].append({"name": content.split(":")[1].strip().strip('"'), "model": "", "provides": [], "requires": []})
            elif "id:" in content and indent == 4:
                data["components"][-1]["id"] = int(content.split(":")[1].strip())
            elif "model:" in content:
                data["components"][-1]["model"] = content.split(":")[1].strip().strip('"')
            elif "provides:" in content:
                pass
            elif content.startswith("- { name:") and indent > 4:
                svc_name = re.search(r'name:\s*"([^"]+)"', content).group(1)
                svc_id = int(re.search(r'id:\s*(\d+)', content).group(1))
                data["components"][-1]["provides"].append({"name": svc_name, "id": svc_id})
            elif "requires:" in content:
                pass
            elif content.startswith("- ") and indent > 4:
                req_name = content.split("-")[1].strip().strip('"')
                data["components"][-1]["requires"].append(req_name)
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
    comp_map = {c["name"]: c for c in data["components"]}
    model_to_lib = {m["name"]: m["lib"] for m in data["models"]}
    global_svc_map = {}
    svc_name_to_id = {}
    comp_to_node = {}
    node_to_uds = {n["name"]: n["uds_path"] for n in data["nodes"]}

    for node in data["nodes"]:
        for thread in node["threads"]:
            for comp_name in thread["components"]:
                if comp_name in comp_map:
                    comp_to_node[comp_name] = node["name"]

    for comp in data["components"]:
        for svc in comp.get("provides", []):
            svc_name = svc["name"]
            global_svc_map[svc_name] = {
                "id": svc["id"],
                "provider_id": comp["id"],
                "node": comp_to_node.get(comp["name"], "Unknown"),
                "uds_path": node_to_uds.get(comp_to_node.get(comp["name"], ""), "")
            }
            svc_name_to_id[svc_name] = svc["id"]

    profile_map = {p["name"]: p["bins"] for p in data["profiles"]}
    for node in data["nodes"]:
        pname = node.get("buffer_profile", "default")
        node["resolved_bins"] = profile_map.get(pname, profile_map.get("default", []))
        node_comps = []
        for thread in node["threads"]:
            resolved_ids, resolved_names, resolved_libs = [], [], []
            for comp_name in thread["components"]:
                comp = comp_map.get(comp_name)
                if not comp: sys.exit(1)
                lib = model_to_lib.get(comp["model"])
                resolved_ids.append(comp["id"])
                resolved_names.append(comp_name)
                resolved_libs.append(lib)
                node_comps.append(comp_name)
            thread["comp_ids"] = resolved_ids
            thread["comp_names"] = resolved_names
            thread["comp_libs"] = resolved_libs

        needed_svc_names = set()
        for comp_name in node_comps:
            comp = comp_map[comp_name]
            for s in comp.get("provides", []): needed_svc_names.add(s["name"])
            for s in comp.get("requires", []): needed_svc_names.add(s)
        
        node["resolved_services"] = [global_svc_map[s] for s in needed_svc_names if s in global_svc_map]

    return {c["name"]: c["id"] for c in data["components"]}, svc_name_to_id

def generate_header(comp_ids, svc_ids, header_path):
    lines = ["#ifndef __NOS_IDS_H__", "#define __NOS_IDS_H__", "", "/* Component IDs */"]
    for name, cid in sorted(comp_ids.items(), key=lambda x: x[1]):
        lines.append(f"#define {to_c_macro(name):20} {cid}")
    lines.append("\n/* Service IDs */")
    for name, sid in sorted(svc_ids.items(), key=lambda x: x[1]):
        lines.append(f"#define {to_c_macro(name):20} {sid}")
    lines.append("\n#endif")
    with open(header_path, 'w') as f: f.write("\n".join(lines))

def generate_node_manifest(node, output_path):
    c_content = ['#include <string.h>', '#include "nos_manifest.h"', '']
    
    c_content.append(f'static const nos_buffer_pool_def_t g_local_pools[] = {{')
    for b in node["resolved_bins"]:
        c_content.append(f'    {{ .chunk_size = {b["size"]}, .chunk_count = {b["count"]} }},')
    c_content.append('    { .chunk_size = 0 }\n};')
    
    c_content.append(f'static const nos_service_def_t g_local_services[] = {{')
    for s in node["resolved_services"]:
        # 注意：这里增加了一个字段用于存放 UDS 路径，以便 node_mgr 能够直接注册远程路由
        c_content.append(f'    {{ .service_id = {s["id"]}, .node_name = "{s["node"]}", .provider_comp_id = {s["provider_id"]}, .remote_uds_path = "{s["uds_path"]}" }},')
    c_content.append(f'    {{ .service_id = 0 }}\n}};')

    c_content.append('const nos_node_def_t g_local_node_def = {')
    c_content.append(f'    .name = "{node["name"]}", .uds_path = "{node["uds_path"]}",')
    c_content.append(f'    .buffer_pools = g_local_pools,')
    c_content.append('    .threads = {')
    for thread in node["threads"]:
        ids_str = ", ".join(map(str, thread["comp_ids"]))
        names_str = ", ".join(f'"{n}"' for n in thread["comp_names"])
        libs_str = ", ".join(f'"{l}"' for l in thread["comp_libs"])
        c_content.append(f'        {{ .name = "{thread["name"]}", .comp_ids = {{{ids_str}, 0}}, .comp_names = {{{names_str}, NULL}}, .comp_models = {{{libs_str}, NULL}} }},')
    c_content.append('        { .name = NULL }')
    c_content.append('    },')
    c_content.append(f'    .services = g_local_services, .service_count = {len(node["resolved_services"])}')
    c_content.append('};')
    
    c_content.append('\nconst nos_node_def_t* nos_manifest_get_local(void) { return &g_local_node_def; }')
    with open(output_path, 'w') as f: f.write("\n".join(c_content))

if __name__ == "__main__":
    if len(sys.argv) < 3: sys.exit(1)
    input_dir, out_h = sys.argv[1], sys.argv[2]
    master = {"nodes": [], "components": [], "profiles": [], "models": []}
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
    
    for node in master["nodes"]:
        out_c = os.path.join(os.path.dirname(out_h), f"manifest_{node['name']}.c")
        out_c = out_c.replace("include", "src/core")
        generate_node_manifest(node, out_c)
        print(node["name"], end=" ") # Output node names for Makefile to capture
