#!/usr/bin/env python3
"""Build summary.json + summary.md from results.tsv + meta.env. Exit 1 on any FAIL."""
import json, sys
from pathlib import Path
out = Path(sys.argv[1])
meta = dict(l.split("=", 1) for l in (out / "meta.env").read_text().splitlines() if "=" in l)
rows = []
for l in (out / "results.tsv").read_text().splitlines():
    n, s, d, lg = (l.split("\t") + [""] * 4)[:4]
    rows.append({"check": n, "status": s, "detail": d, "log": lg})
cnt = {}
for r in rows: cnt[r["status"]] = cnt.get(r["status"], 0) + 1
overall = "FAIL" if cnt.get("FAIL") else "PASS"
mode = "require-fixed" if meta.get("REQUIRE_FIXED") == "1" else "default (acceptance failures = expected-fail)"
doc = {"overall": overall, "mode": mode, "pinned_sha": meta.get("SHA"), "source": meta.get("SRC_KIND", "") + " " + meta.get("SRC_DESC", ""),
       "library_version": meta.get("LIBVER"), "dirty": meta.get("DIRTY"), "started_utc": meta.get("STARTED"), "finished_utc": meta.get("FINISHED"),
       "host": meta.get("HOST"), "os": meta.get("OS"), "cxx": meta.get("CXX_VERSION"), "python": meta.get("PY_VERSION"),
       "arduino_cli": meta.get("ARDUINO_CLI"), "cores": meta.get("CORES") or None, "counts": cnt, "checks": rows}
(out / "summary.json").write_text(json.dumps(doc, indent=1) + "\n")
md = [f"# Si5351Arduino v2.3.1 checks: **{overall}**", "",
      f"- Pinned SHA: `{doc['pinned_sha']}` (library.properties version {doc['library_version'] or '?'})",
      f"- Source: {doc['source']}" + (f" (dirty: {doc['dirty']})" if doc['dirty'] not in ("n/a", "no", None) else ""),
      f"- Mode: {mode}", f"- Started {doc['started_utc']}, finished {doc['finished_utc']} (UTC)",
      f"- Host: {doc['host']} ({doc['os']}); {doc['cxx']}; {doc['python']}" + (f"; {doc['arduino_cli']}" if doc['arduino_cli'] else ""),
      f"- Counts: " + ", ".join(f"{k} {v}" for k, v in sorted(cnt.items())), "",
      "| Check | Status | Detail | Log |", "|---|---|---|---|"]
for r in rows:
    md.append(f"| {r['check']} | {'**FAIL**' if r['status']=='FAIL' else r['status']} | {r['detail'].replace('|','/')} | {r['log']} |")
md += ["", "Statuses: PASS; FAIL = unexpected failure (exit 1); XFAIL = expected failure (v2.3.1 acceptance tests in default mode, known v2.3.0 vcxo example bug); SKIP = not applicable to this source; INFO = reported, not gating."]
(out / "summary.md").write_text("\n".join(md) + "\n")
print(f"OVERALL {overall}: " + ", ".join(f"{k} {v}" for k, v in sorted(cnt.items())))
sys.exit(1 if overall == "FAIL" else 0)
