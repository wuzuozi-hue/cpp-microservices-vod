import subprocess
import sys
from fdfs_fixture import isolated_fdfs

tracker, storage, monitor, test = sys.argv[1:]
with isolated_fdfs(tracker, storage, monitor) as address:
    subprocess.run([test, address], check=True, timeout=30)
