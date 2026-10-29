#!/usr/bin/env python3
"""Writes SHA256SUMS and manifest.json for a release (SB-6001).

The installer reads manifest.json from releases/latest/download/, so it carries everything it
needs without the GitHub API: the version, the VPX build the plugin was compiled against, and per
platform the file, its download URL and its sha256. The pin comes from cmake/vpx_headers.cmake,
so the manifest cannot disagree with what was built.

usage: release-manifest.py <dist-dir> <version> <repo e.g. scorbit-io/vpx-scorbit-plugin>
"""
import hashlib, json, pathlib, re, sys, zipfile

dist, version, repo = pathlib.Path(sys.argv[1]), sys.argv[2], sys.argv[3]
root = pathlib.Path(__file__).resolve().parent.parent
pins = (root / "cmake" / "vpx_headers.cmake").read_text()


def pin(name):
    m = re.search(rf'^set\({name} "([^"]+)"\)', pins, re.M)
    if not m:
        sys.exit(f"cmake/vpx_headers.cmake has no {name}")
    return m.group(1)


prefix = f"vpx-scorbit-plugin-{version}-"
zips = sorted(dist.glob(f"{prefix}*.zip"))
if not zips:
    sys.exit(f"no {prefix}*.zip in {dist}")

def zipped_version(z):
    with zipfile.ZipFile(z) as f:
        try:
            cfg = f.read("scorbit/plugin.cfg").decode("utf-8")
        except KeyError:
            sys.exit(f"{z.name} has no scorbit/plugin.cfg")
    m = re.search(r'^version = "([^"]+)"\r?$', cfg, re.M)
    return m.group(1) if m else None


assets, sums = [], []
for z in zips:
    # The installer refuses a zip whose plugin.cfg disagrees with manifest.json as damaged.
    if (found := zipped_version(z)) != version:
        sys.exit(f"{z.name}: scorbit/plugin.cfg version is {found}, the manifest says {version}")
    digest = hashlib.sha256(z.read_bytes()).hexdigest()
    sums.append(f"{digest}  {z.name}")
    assets.append({
        "platform": z.name[len(prefix):-len(".zip")],
        "file": z.name,
        "url": f"https://github.com/{repo}/releases/download/v{version}/{z.name}",
        "sha256": digest,
    })

manifest = {
    "version": version,
    "vpx_plugin_api_commit": pin("VPX_PLUGIN_API_COMMIT"),
    # The plugin API has no version handshake: older VPX builds corrupt memory at table start.
    "vpx_min_revision": int(pin("VPX_PINNED_REVISION")),
    "assets": assets,
}
(dist / "SHA256SUMS").write_text("\n".join(sums) + "\n")
(dist / "manifest.json").write_text(json.dumps(manifest, indent=2) + "\n")
print(json.dumps(manifest, indent=2))
