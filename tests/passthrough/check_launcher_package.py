"""Run the unmodified shipping scanner on the actual stripped launcher."""
import os,pathlib,shutil,subprocess,sys,tempfile
root=pathlib.Path(__file__).resolve().parents[2];sys.path.insert(0,str(root))
from tools.package_release import scan, remap_pe_source_names
source,strip=pathlib.Path(sys.argv[1]).resolve(),sys.argv[2]
with tempfile.TemporaryDirectory(prefix="venture-mr-launcher-") as temporary:
    layout=pathlib.Path(temporary);target=layout/source.name;shutil.copy2(source,target)
    if target.suffix.lower() == ".exe": remap_pe_source_names(target)
    subprocess.run([strip,"--strip-all",str(target)],check=True)
    scan(layout,[str(root)],["McFredward",os.environ.get("USERNAME",""),os.environ.get("USER","")],[os.environ.get("COMPUTERNAME","")])
print("PASS actual launcher with the upstream shipping scanner and McFredward build identity")
