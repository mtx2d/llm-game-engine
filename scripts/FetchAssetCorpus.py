"""Fetch pinned Khronos validation assets and licenses into the ignored build tree."""

import concurrent.futures
import hashlib
import json
from pathlib import Path
import urllib.request


def main():
    repository = Path(__file__).resolve().parents[1]
    manifest = json.loads((repository / "Tests/AssetCorpus.lock.json").read_text())
    destination = repository / "build/asset-corpus"

    def fetch(item):
        path = destination / item["Path"]
        path.resolve().relative_to(destination.resolve())
        if path.is_file() and hashlib.sha256(path.read_bytes()).hexdigest() == item["SHA256"]:
            return
        with urllib.request.urlopen(item["URL"], timeout=90) as response:
            data = response.read(32 * 1024 * 1024 + 1)
        if hashlib.sha256(data).hexdigest() != item["SHA256"]:
            raise RuntimeError(f"Asset digest mismatch: {item['Path']}")
        path.parent.mkdir(parents=True, exist_ok=True)
        temporary = path.with_name(path.name + ".download")
        temporary.write_bytes(data)
        temporary.replace(path)

    files = manifest["Licenses"] + [file for model in manifest["Models"] for file in model["Files"]]
    with concurrent.futures.ThreadPoolExecutor(max_workers=4) as executor:
        list(executor.map(fetch, files))
    print(f"Verified {len(manifest['Models'])} glTF models and their notices at {destination}")


if __name__ == "__main__":
    main()
