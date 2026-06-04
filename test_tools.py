import subprocess, sys, json, os

os.chdir(os.path.dirname(os.path.abspath(__file__)))

def list_tools(server_dir, label):
    p = subprocess.Popen(
        [sys.executable, "-m", "src.server"],
        cwd=server_dir,
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
    )
    req = json.dumps({"jsonrpc":"2.0","id":1,"method":"tools/list","params":{}}) + "\n"
    try:
        out, err = p.communicate(req.encode("utf-8"), timeout=10)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()

    print(f"\n=== {label} ===")
    for line in out.decode("utf-8", errors="replace").split("\n"):
        try:
            d = json.loads(line.strip())
            tools = d.get("result", {}).get("tools", [])
            print(f"工具数量: {len(tools)}")
            for t in tools:
                print(f"  {t['name']}")
            return
        except:
            pass

    if err:
        print(f"stderr: {err.decode('utf-8', errors='replace')[:500]}")

list_tools("ce-mcp", "ce-mcp")
list_tools("dnlib-mcp", "dnlib-mcp")
