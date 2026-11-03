"""Compare ordinary VR SPIR-V against the retained upstream shader using one compiler."""
import pathlib,subprocess,sys,tempfile
compiler,generated=sys.argv[1],pathlib.Path(sys.argv[2])
with tempfile.TemporaryDirectory(prefix="venture-vr-shader-") as temporary:
    for name,defines in (("frag",[]),("ms_frag",["-DMULTISAMPLE=1"])):
        target=pathlib.Path(temporary)/(name+".spv")
        subprocess.run([compiler,"-V","--target-env","vulkan1.0",*defines,
                        str(pathlib.Path(__file__).with_name("ordinary_color.frag")),"-o",str(target)],check=True)
        if target.read_bytes() != (generated/("color_"+name+".spv")).read_bytes():
            raise SystemExit("Ordinary VR shader changed: "+name)
print("PASS ordinary single/multisample shaders are byte-identical to upstream")
