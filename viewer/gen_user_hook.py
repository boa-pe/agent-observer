#!/usr/bin/env python3
"""Generate viewer/copilot-user-hooks.json (allowlisted payloads for the local wall)."""

from __future__ import annotations

import json
from pathlib import Path

URL = "http://127.0.0.1:8770/api/hook"
EVENTS = [
    "sessionStart",
    "userPromptSubmitted",
    "preToolUse",
    "postToolUse",
    "permissionRequest",
    "notification",
    "subagentStart",
    "subagentStop",
    "agentStop",
    "errorOccurred",
    "sessionEnd",
]


def bash_cmd(ev: str) -> str:
    """python3 strips stdin; without python, POST ?event= with empty body."""
    # exec("...\n...") keeps the -c argument on one line for the hook JSON.
    py = (
        "exec(\"import sys,json,os,urllib.request as U\\n"
        "j=json.loads(sys.stdin.read() or '{}')\\n"
        "p=os.path.abspath(j.get('cwd') or os.getcwd())\\n"
        "repo=os.path.basename(p) or 'repo'\\n"
        "q=p\\n"
        "for _ in range(64):\\n"
        " g=os.path.join(q,'.git')\\n"
        " if os.path.isdir(g) or os.path.isfile(g):\\n"
        "  repo=os.path.basename(q) or repo; break\\n"
        " n=os.path.dirname(q)\\n"
        " if n==q: break\\n"
        " q=n\\n"
        f"o={{'event':'{ev}','repo':repo}}\\n"
        "t=j.get('toolName') or j.get('tool_name') or ''\\n"
        "s=j.get('sessionId') or j.get('session_id') or ''\\n"
        "o.update({{k:v for k,v in (('tool',t),('session',s)) if v}})\\n"
        f"req=U.Request('{URL}',data=json.dumps(o).encode(),"
        "headers={{'Content-Type':'application/json'}},method='POST')\\n"
        "try:\\n U.urlopen(req,timeout=2)\\nexcept Exception:\\n pass\")"
    )
    fallback = (
        f"curl -s -m 2 --connect-timeout 1 -X POST \"{URL}?event={ev}\" "
        f">/dev/null 2>&1 || true"
    )
    return f"python3 -c '{py}' 2>/dev/null || {fallback}"


def ps_cmd(ev: str) -> str:
    return (
        "$OutputEncoding=[Text.UTF8Encoding]::new($false); "
        "try{$j=$input|ConvertFrom-Json}catch{$j=$null}; "
        "$cwd=if($j -and $j.cwd){[string]$j.cwd}else{(Get-Location).Path}; "
        "$p=$cwd; $repo=Split-Path -Leaf $p; "
        "while($p){ "
        "if(Test-Path -LiteralPath (Join-Path $p '.git')){ $repo=Split-Path -Leaf $p; break }; "
        "$n=Split-Path -Parent $p; if(-not $n -or $n -eq $p){break}; $p=$n "
        "}; "
        "$tool=''; $sid=''; "
        "if($j){ "
        "if($j.toolName){$tool=[string]$j.toolName}elseif($j.tool_name){$tool=[string]$j.tool_name}; "
        "if($j.sessionId){$sid=[string]$j.sessionId}elseif($j.session_id){$sid=[string]$j.session_id} "
        "}; "
        f"$body=[ordered]@{{event='{ev}';repo=$repo}}; "
        "if($tool){$body.tool=$tool}; if($sid){$body.session=$sid}; "
        "$json=($body|ConvertTo-Json -Compress); "
        "[Console]::OutputEncoding=[Text.UTF8Encoding]::new($false); "
        f"$json | curl.exe -s -m 2 --connect-timeout 1 -X POST -H \"Content-Type: application/json\" "
        f"-H \"Expect:\" --data-binary \"@-\" \"{URL}\" | Out-Null; exit 0"
    )


def main() -> None:
    hooks = {
        ev: [{
            "type": "command",
            "bash": bash_cmd(ev),
            "powershell": ps_cmd(ev),
            "timeoutSec": 5,
        }]
        for ev in EVENTS
    }
    doc = {
        "_comment": (
            "GitHub Copilot user-level hooks for the local Agent Observer viewer. "
            "Save as %USERPROFILE%\\.copilot\\hooks\\agent-observer.json "
            "(or ~/.copilot/hooks/ on macOS/Linux; $COPILOT_HOME/hooks/ if set). "
            "Posts only event, tool name, repo directory name, and session id to "
            f"{URL} — never prompt, tool args, results, error text, or full cwd. "
            "Always exits 0. Run the viewer with: python viewer/server.py"
        ),
        "version": 1,
        "hooks": hooks,
    }
    out = Path(__file__).resolve().parent / "copilot-user-hooks.json"
    out.write_text(json.dumps(doc, indent=2) + "\n", encoding="utf-8")
    print(f"wrote {out}")


if __name__ == "__main__":
    main()
