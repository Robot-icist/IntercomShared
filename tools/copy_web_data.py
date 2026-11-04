import os
import shutil
import filecmp

Import("env")

# Source: shared web data directory
shared_dir = os.path.join(env.subst("$PROJECT_DIR"), "..", "IntercomShared", "web", "data")
# Fallback: try relative to library extra dirs
if not os.path.isdir(shared_dir):
    shared_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "web", "data")

# Destination: project data directory
project_dir = env.subst("$PROJECT_DIR")
data_dir = os.path.join(project_dir, "data")

if not os.path.isdir(shared_dir):
    print("[copy_web_data] Shared web data not found at:", shared_dir)
    print("[copy_web_data] Skipping copy")
else:
    # Create data dir if it doesn't exist
    os.makedirs(data_dir, exist_ok=True)

    # Only deploy browser assets; certificate private keys never belong in the web root.
    for name in ("index.html", "app.js", "style.css", "manifest.json"):
        src_file = os.path.join(shared_dir, name)
        if os.path.isfile(src_file):
            shutil.copy2(src_file, os.path.join(data_dir, name))
            print("[copy_web_data] Copied:", name)

    # Remove certificate files copied by older versions of this script.
    for name in ("cert.pem", "key.pem"):
        src_file = os.path.join(shared_dir, name)
        dst_file = os.path.join(data_dir, name)
        if (os.path.isfile(src_file) and os.path.isfile(dst_file) and
                filecmp.cmp(src_file, dst_file, shallow=False)):
            os.remove(dst_file)
            print("[copy_web_data] Removed previously copied:", name)

    print("[copy_web_data] Done. Web data ready in:", data_dir)
