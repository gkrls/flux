#!/usr/bin/env python3

import os, json, sys, argparse
from datetime import datetime

def _make_config_defines(cfg: dict) -> str:
    ls = []
    for k, v in cfg.items():
        ls.append(f"#define {str(k).upper()} {(v.upper() if isinstance(v,str) else str(v))}")
    return "\n".join(ls) + "\n\n"

def _make_platform_defines(tofino, asic, pipes) -> str:
    ls = []
    ls.append(f"#define TOFINO {tofino}")
    ls.append(f"#define TOFINO_MODEL {1 if not asic else 0}")
    if not isinstance(pipes, list):
        raise ValueError("invalid pipes value")
    if len(pipes) != 2 and len(pipes) != 4:
        raise ValueError("number of pipes hould be 2 or 4")
    for i, p in enumerate(pipes):
        try: n = int(p)
        except Exception: raise ValueError("invalid pipe number")
        ls.append(f"#define PIPE_{i} {n}")
    return "\n".join(ls) + "\n\n"

def _resolve_main(main_path: str, config_dir: str, install_dir: str) -> str:
    # absolute
    if os.path.isabs(main_path) and os.path.exists(main_path):
        return os.path.abspath(main_path)
    # relative to config dir
    cand1 = os.path.join(config_dir, main_path)
    if os.path.exists(cand1): return os.path.abspath(cand1)
    # relative to install dir
    cand2 = os.path.join(install_dir, main_path)
    if os.path.exists(cand2): return os.path.abspath(cand2)
    raise FileNotFoundError(f"Main P4 not found; tried: {cand1}, {cand2}")

def generate_p4_program(config_path, output_path, install_dir, verbose=False):
    config_path = os.path.abspath(config_path)
    config_dir  = os.path.dirname(config_path)
    if not os.path.exists(config_path):
        raise FileNotFoundError(f"Config not found: {config_path}")

    cfg = json.load(open(config_path))
    prog = cfg['switch']['program']
    straggle_aware = "straggle_aware" in cfg['switch']["program"] and cfg['switch']["program"]["straggle_aware"]
    main_path = prog['main']
    config_value = prog['config']

    tofino = cfg['switch']['tofino'] if 'tofino' in cfg['switch'] else 1 #cfg.get('tofino', 1)
    asic = cfg['switch']['asic'] if 'asic' in cfg['switch'] else False #in cfg.get('asic', False) #cfg["switch"]["asic"] #cfg.get('asic', {})
    pipes = cfg["switch"]["pipes"] # cfg.get('switch', {}).get('pipes', [])

    abs_main = _resolve_main(main_path, config_dir, install_dir)

    # output: absolute (relative to CWD if not absolute)
    output_path = os.path.abspath(output_path)
    os.makedirs(os.path.dirname(output_path), exist_ok=True)

    ts = datetime.utcnow().strftime('%Y-%m-%d %H:%M:%S UTC')
    out = []
    out += [f"// File '{output_path}'",
            f"// generated at {ts}",
            f"// from config '{config_path}'\n"]

    if isinstance(config_value, dict):
        mode = "RAW"
        out.append("#define DPA_CONFIG_RAW 1\n")
        if not straggle_aware: out.append("#define DPA_NS 1\n")
        out.append(_make_platform_defines(tofino, asic, pipes))
        out.append(_make_config_defines(config_value))
    elif isinstance(config_value, str):
        if config_value.endswith(".p4"):
            mode = "FILE"
            abs_cfg = _resolve_main(config_value, config_dir, install_dir)
            out.append(f'#define DPA_CONFIG "{abs_cfg}"\n')
        else:
            mode = "IDENT"
            out.append(f"#define DPA_CONFIG_{config_value.upper()}\n")
    else:
        raise TypeError("program.config must be string or object")

    out.append(f'#include "{abs_main}"\n')
    open(output_path, 'w').write("\n".join(out))

    if verbose:
        print("p4gen:")
        print(f"  install   : {install_dir}")
        print(f"  main      : {abs_main}")
        print(f"  mode      : {mode}")
        print(f"  output    : {output_path}")

if __name__ == '__main__':
    install_dir = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))

    ap = argparse.ArgumentParser(description="Generate DPA switch P4 (inline/raw config)")
    ap.add_argument("-c","--config", required=True, help="Config JSON file")
    ap.add_argument("-o","--output", required=True, help="Output P4 file (relative to CWD if not absolute)")
    ap.add_argument("-v","--verbose", action="store_true", help="Print details")
    args = ap.parse_args()

    try:
        generate_p4_program(args.config, args.output, install_dir, verbose=args.verbose)
        # if args.verbose:
        #     print(f"dpa-p4gen: {os.path.abspath(args.output)}")
    except Exception as e:
        print(f"dpa-p4gen: error: {e}", file=sys.stderr); sys.exit(1)
