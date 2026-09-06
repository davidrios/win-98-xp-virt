"""Emit the third-party crate listing for THIRD-PARTY-NOTICES.md, grouped by
declared licence, one section per shipped binary. Read straight from cargo
metadata so the file can be regenerated rather than hand-maintained."""
import collections
import json
import sys

m = json.load(open(sys.argv[1]))
pkgs = {p["id"]: p for p in m["packages"]}
nodes = {n["id"]: n for n in m["resolve"]["nodes"]}
workspace = {p["name"] for p in m["packages"] if p.get("source") is None}


def tree(binary):
    root = next(i for i, p in pkgs.items() if p["name"] == binary)
    seen, stack = set(), [root]
    while stack:
        i = stack.pop()
        if i in seen:
            continue
        seen.add(i)
        for d in nodes[i]["deps"]:
            kinds = {k.get("kind") for k in d["dep_kinds"]}
            if None in kinds or "build" in kinds:
                stack.append(d["pkg"])
    return seen


for binary in sys.argv[2:]:
    lic = collections.defaultdict(set)
    for i in tree(binary):
        p = pkgs[i]
        if p["name"] in workspace:
            continue
        lic[p.get("license") or "(see the crate's own licence file)"].add(p["name"])
    total = sum(len(v) for v in lic.values())
    print("\n### `%s` — %d third-party crates\n" % (binary, total))
    for k in sorted(lic, key=lambda k: (-len(lic[k]), k)):
        names = ", ".join("`%s`" % n for n in sorted(lic[k]))
        print("**%s** (%d): %s\n" % (k, len(lic[k]), names))
