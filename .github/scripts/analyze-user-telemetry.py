#!/usr/bin/env python3
"""Analyze user-reviewed ISO-WCX diagnostics from GitHub Issues before builds.

This is an aggregate-only preflight. Untrusted issue text is sanitized and
cannot execute code or, by itself, block CI. A confirmed regression label
requires maintainer action. No client token or raw logs are uploaded.
"""
from __future__ import annotations
import argparse
from collections import defaultdict
from datetime import datetime, timedelta, timezone
import json
import os
from pathlib import Path
import re
from urllib.request import Request, urlopen

FORMATS = {"iso", "udf", "wim", "esd", "swm", "vhd", "vhdx", "img", "other"}
ACTIONS = {"open", "close", "pack", "delete", "extract", "test", "skip"}
REASONS = {"none", "cancelled", "integrity_failed", "verification_failed",
           "out_of_space", "access_denied", "protected_layout",
           "missing_input", "unsupported_layout", "io_error",
           "unexpected_exception"}
REGRESSION_LABEL = "iso-wcx-regression-confirmed"
TITLE_PREFIX = "ISO-WCX Diagnostics:"
TABLE = re.compile(
    r"^\|\s*([A-Za-z0-9_.() +-]{1,64})\s*\|\s*"
    r"(iso|udf|wim|esd|swm|vhd|vhdx|img|other)\s*\|\s*"
    r"(open|close|pack|delete|extract|test|skip)\s*\|\s*"
    r"(-?\d{1,5})\s*\|\s*([a-z_]{1,40})\s*\|\s*(\d{1,7})\s*\|$",
    re.IGNORECASE,
)

def load_issues(repo: str, token: str) -> list[dict]:
    if not re.fullmatch(r"[A-Za-z0-9_.-]+/[A-Za-z0-9_.-]+", repo):
        raise ValueError("Invalid repository")
    url = f"https://api.github.com/repos/{repo}/issues?state=open&sort=updated&per_page=100"
    headers = {"Accept": "application/vnd.github+json",
               "User-Agent": "ISO-WCX-Prebuild-Diagnostics"}
    if token:
        headers["Authorization"] = "Bearer " + token
    req = Request(url, headers=headers)
    with urlopen(req, timeout=15) as response:
        raw = response.read(1024 * 1024 + 1)
    if len(raw) > 1024 * 1024:
        raise ValueError("Issue list response is too large")
    result = json.loads(raw)
    if not isinstance(result, list):
        raise ValueError("Unexpected Issues API response")
    return result

def analyze(issues: list[dict], *, available: bool = True) -> dict:
    cutoff = datetime.now(timezone.utc) - timedelta(days=30)
    categories = defaultdict(int)
    matched = 0
    confirmed = set()
    for item in issues[:100]:
        if not isinstance(item, dict) or "pull_request" in item:
            continue
        title = str(item.get("title", ""))
        if not title.startswith(TITLE_PREFIX):
            continue
        try:
            created = datetime.fromisoformat(str(item["created_at"]).replace("Z", "+00:00"))
        except (KeyError, ValueError, TypeError):
            continue
        if created < cutoff:
            continue
        labels = {str(x.get("name", "")) for x in item.get("labels", []) if isinstance(x, dict)}
        verified = REGRESSION_LABEL in labels
        body = str(item.get("body") or "")[:10000]
        matched += 1
        for line in body.splitlines()[:150]:
            match = TABLE.fullmatch(line.strip())
            if not match:
                continue
            build, fmt, action, code_s, reason, count_s = match.groups()
            reason = reason.lower()
            fmt, action = fmt.lower(), action.lower()
            if reason not in REASONS or fmt not in FORMATS or action not in ACTIONS:
                continue
            code = int(code_s)
            count = int(count_s)
            if count <= 0 or count > 1000000 or code < -32768 or code > 32767:
                continue
            key = (build.strip(), fmt, action, code, reason)
            categories[key] += count
            if verified:
                confirmed.add(key)
    rows = [
        {"build":build, "format":fmt, "action":action, "code":code,
         "reason":reason, "count":count, "confirmed":key in confirmed}
        for key,count in categories.items()
        for build,fmt,action,code,reason in [key]
    ]
    rows.sort(key=lambda row:(-row["count"],row["build"],row["action"]))
    critical = [
        row for row in rows
        if row["confirmed"] and row["count"] >= 3 and row["code"] not in (0,21,24)
        and row["reason"] in ("integrity_failed", "verification_failed",
                              "unexpected_exception")
    ]
    return {"schema":1, "source_available":available, "issues":matched,
            "operation_results":sum(row["count"] for row in rows),
            "critical_confirmed":critical, "groups":rows[:40]}

def markdown(result: dict) -> str:
    output=["## ISO-WCX — pre-build user diagnostics", "",
            "Source: public GitHub Issues, user-reviewed summaries (last 30 days).",
            f"Issues analyzed: **{result['issues']}**",
            f"Operation results: **{result['operation_results']}**",
            f"Confirmed critical regressions: **{len(result['critical_confirmed'])}**", ""]
    if not result["source_available"]:
        output.append("**Telemetry unavailable — no clean bill of health.**")
    elif not result["groups"]:
        output.append("No submitted operation summaries matched the format.")
    else:
        output += ["| Build | Format | Action | WCX code | Reason | Count | Confirmed |",
                   "|---|---|---|---:|---|---:|---|"]
        for row in result["groups"][:20]:
            output.append("| {build} | {format} | {action} | {code} | {reason} | {count} | {confirmed} |".format(**row))
    output.append("\n**Untrusted reports do not block builds.** Only maintainer-confirmed, repeated critical regressions can block the build.")
    return "\n".join(output)+"\n"

def main() -> int:
    p = argparse.ArgumentParser()
    p.add_argument("--fixture", help="Fixture of GitHub issue objects for local tests")
    p.add_argument("--output", default="user-telemetry-analysis.json")
    p.add_argument("--fail-confirmed", action="store_true")
    args = p.parse_args()
    available = True
    try:
        if args.fixture:
            issues = json.loads(Path(args.fixture).read_text(encoding="utf-8"))
        else:
            issues = load_issues(os.environ.get("GITHUB_REPOSITORY","sgennadi/ISO-WCX"),
                                 os.environ.get("GITHUB_TOKEN",""))
    except Exception as exc:
        # Missing network must not masquerade as "zero errors" and must
        # not prevent ordinary code compilation in a developer PR.
        available = False
        issues = []
        print("::warning::Diagnostics API unavailable: "+type(exc).__name__)
    result = analyze(issues, available=available)
    summary = markdown(result)
    print(summary)
    Path(args.output).write_text(json.dumps(result,indent=2)+"\n",encoding="utf-8")
    if os.environ.get("GITHUB_STEP_SUMMARY"):
        with open(os.environ["GITHUB_STEP_SUMMARY"],"a",encoding="utf-8") as stream:
            stream.write(summary)
    return 2 if args.fail_confirmed and result["critical_confirmed"] else 0

if __name__ == "__main__":
    raise SystemExit(main())
