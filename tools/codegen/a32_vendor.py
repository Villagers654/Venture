#!/usr/bin/env python3
"""First Contact-only vendor ABI inventory. No implementation or SDK binary code.

VrApi base-PCS signatures are declared below (1.21 API); layout/handle entries
remain adapter-required. Platform signatures use the already pinned SDK inputs.
"""
import argparse
import csv
import hashlib
import json
import re
from pathlib import Path
from a32_descriptors import ROOT, classify, emit

# VrApi 1.21 VrApi.h, VrApi_Input.h, VrApi_SystemUtils.h, VrApi_Vulkan.h.
# L: pointers to wire records, H: opaque Token32, C: by-value composite (sret).
VR = '''
vrapi_Initialize I32 L:ovrInitParms
vrapi_Shutdown Void
vrapi_GetTimeInSeconds F64
vrapi_EnterVrMode H:ovrMobile L:ovrModeParms
vrapi_LeaveVrMode Void H:ovrMobile
vrapi_GetPredictedDisplayTime F64 H:ovrMobile I64
vrapi_GetPredictedTracking2 C:ovrTracking2:360 H:ovrMobile F64
vrapi_GetPredictedTracking C:ovrTracking:104 H:ovrMobile F64
vrapi_SubmitFrame Void H:ovrMobile L:ovrFrameParms
vrapi_SubmitFrame2 I32 H:ovrMobile L:ovrSubmitFrameDescription2
vrapi_GetInstanceExtensionsVulkan I32 Ptr Ptr
vrapi_GetDeviceExtensionsVulkan I32 Ptr Ptr
vrapi_CreateSystemVulkan I32 L:ovrSystemCreateInfoVulkan
vrapi_GetSystemPropertyInt I32 L:ovrJava I32
vrapi_GetSystemPropertyFloat F32 L:ovrJava I32
vrapi_GetSystemPropertyFloatArray I32 L:ovrJava I32 Ptr I32
vrapi_GetSystemStatusInt I32 L:ovrJava I32
vrapi_GetSystemStatusFloat F32 L:ovrJava I32
vrapi_GetPropertyInt U8 L:ovrJava I32 Ptr
vrapi_SetPropertyInt Void L:ovrJava I32 I32
vrapi_SetPropertyFloat Void L:ovrJava I32 F32
vrapi_SetRemoteEmulation I32 H:ovrMobile U8
vrapi_RecenterPose Void H:ovrMobile
vrapi_ShowSystemUI U8 L:ovrJava I32
vrapi_SetPerfThread I32 H:ovrMobile I32 U32
vrapi_SetClockLevels I32 H:ovrMobile I32 I32
vrapi_SetExtraLatencyMode I32 H:ovrMobile I32
vrapi_SetDisplayRefreshRate I32 H:ovrMobile F32
vrapi_SetTrackingSpace I32 H:ovrMobile I32
vrapi_LocateTrackingSpace C:ovrPosef:28 H:ovrMobile I32
vrapi_EnumerateInputDevices I32 H:ovrMobile U32 L:ovrInputCapabilityHeader
vrapi_GetInputDeviceCapabilities I32 H:ovrMobile L:ovrInputCapabilityHeader
vrapi_GetInputTrackingState I32 H:ovrMobile U32 F64 L:ovrTracking
vrapi_GetCurrentInputState I32 H:ovrMobile U32 L:ovrInputStateHeader
vrapi_SetHapticVibrationSimple I32 H:ovrMobile U32 F32
vrapi_SetHapticVibrationBuffer I32 H:ovrMobile U32 L:ovrHapticBuffer
vrapi_GetBoundaryTriggerState I32 H:ovrMobile I32 L:ovrBoundaryTriggerResult
vrapi_TestPointIsInBoundary I32 H:ovrMobile C:ovrVector3f:12 Ptr L:ovrBoundaryTestResult
vrapi_GetBoundaryGeometry I32 H:ovrMobile U32 Ptr L:ovrVector3f
vrapi_GetBoundaryOrientedBoundingBox I32 H:ovrMobile L:ovrPosef L:ovrVector3f
vrapi_GetBoundaryVisible I32 H:ovrMobile Ptr
vrapi_RequestBoundaryVisible I32 H:ovrMobile U8
vrapi_CreateTextureSwapChain H:ovrTextureSwapChain I32 I32 I32 I32 I32 U8
vrapi_DestroyTextureSwapChain Void H:ovrTextureSwapChain
vrapi_GetTextureSwapChainLength I32 H:ovrTextureSwapChain
vrapi_GetTextureSwapChainHandle U32 H:ovrTextureSwapChain I32
vrapi_SetTextureSwapChainHandle Void H:ovrTextureSwapChain I32 U32
vrapi_GetTextureSwapChainBufferVulkan I32 H:ovrTextureSwapChain I32 L:VkImage
vrapi_CreateAndroidSurfaceSwapChain H:ovrTextureSwapChain I32 I32
vrapi_GetTextureSwapChainAndroidSurface H:jobject H:ovrTextureSwapChain
vrintegration_Initialize I32 L:Init28
vrintegration_Shutdown Void
vrintegration_ClearState I32 U32
vrintegration_SetThreadPerformance I32 U32 I32
vrintegration_AutoThreadScheduling I32 U32 Ptr Ptr I32
'''

def vr_type(s):
    parts = s.split(':')
    return dict(kind={'L': 'Layout', 'H': 'Handle', 'C': 'Composite'}.get(parts[0], parts[0]),
                tag=parts[1] if len(parts) > 1 else '', size=int(parts[2]) if len(parts) > 2 else 0,
                alignment=8 if 'Tracking' in s else 4)

def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--imports', type=Path, required=True)
    p.add_argument('--output', type=Path, default=ROOT / 'src/runtime/core/generated')
    a = p.parse_args()
    wanted = {(r['provider'].split(' [')[0], r['symbol']) for r in csv.DictReader(a.imports.open())}
    rows = {}
    for line in VR.strip().splitlines():
        name, result, *args = line.split()
        lib = 'libvrintegrationloader.so' if name.startswith('vrintegration') else 'libvrapi.so'
        if (lib, name) in wanted or name == 'vrapi_GetPredictedTracking':
            rows[(lib, name)] = dict(library=lib, symbol=name, result=vr_type(result), arguments=list(map(vr_type, args)), source='VrApi 1.21 API / First Contact OVRPlugin')
    pins = json.loads((ROOT / 'src/runtime/hle/ovrplatform/sdk_sources.json').read_text())
    signatures = {}
    enum_types = set()
    aliases = {'ovrID': 'uint64_t', 'ovrRequest': 'uint64_t'}
    for source in pins['inputs']:
        path = ROOT / '.deps' / source['path']
        blob = path.read_bytes()
        assert hashlib.sha256(blob).hexdigest() == source['sha256'], path
        text = re.sub(r'/\*.*?\*/|//[^\n]*', '', blob.decode('utf-8-sig'), flags=re.S)
        if path.suffix == '.h':
            enum_types.update(re.findall(r'typedef\s+enum\s+\w*\s*\{[^}]*\}\s*(\w+)\s*;', text, re.S))
            for result, name, args in re.findall(r'OVRP[L]?_PUBLIC_FUNCTION\((.*?)\)\s+(ovr_\w+)\s*\((.*?)\)\s*;', text, re.S):
                signatures[name] = (result.strip(), args.strip(), path.name, False)
        else:
            for result, name, args in re.findall(r'(?:public|private)\s+static\s+extern\s+(\S+)\s+(ovr_\w+)\((.*?)\);', text, re.S):
                signatures.setdefault(name.removesuffix('_Native'), (result, args, path.name, True))
    def platform_type(t, managed):
        t = t.strip().removeprefix('const ')
        if t.endswith('Handle') or t == 'IntPtr': return dict(kind='Handle', tag=t)
        if 'Callback' in t: return dict(kind='Callback', tag=t)
        if managed:
            t = {'UInt64': 'uint64_t', 'ulong': 'uint64_t', 'UInt32': 'unsigned int', 'uint': 'unsigned int', 'UIntPtr': 'size_t', 'Int64': 'int64_t', 'long': 'int64_t', 'string': 'const char *', 'short': 'short'}.get(t, t)
        if t in enum_types:
            return dict(kind='I32', tag=t) # public Platform enum
        return classify(t, aliases)
    for (lib, name) in wanted:
        if lib != 'libovrplatformloader.so' or name not in signatures: continue
        result, args, source, managed = signatures[name]
        types = []
        for arg in args.split(',') if args and args != 'void' else []:
            # Remove argument identifier, retaining trailing * and array markers.
            t = re.sub(r'\s*\b\w+\s*$', '', arg.strip())
            types.append(platform_type(t, managed))
        rows[(lib, name)] = dict(library=lib, symbol=name, result=platform_type(result, managed), arguments=types, source='Pinned Oculus Platform SDK ' + source)
    # OVR_Types.h declares helpers without PUBLIC_FUNCTION. CAPI.cs declares
    # Log_NewEvent's string-pair array, which the generic managed parser cannot
    # distinguish from a scalar handle.
    manual = {
        'ovr_Log_NewEvent': ({'kind': 'Void'}, [{'kind': 'Ptr', 'tag': 'const char *', 'size': 1}, {'kind': 'Layout', 'tag': 'const char *const *'}, {'kind': 'Size'}]),
        'ovrID_FromString': ({'kind': 'U8'}, [{'kind': 'Ptr', 'tag': 'ovrID *', 'size': 8}, {'kind': 'Ptr', 'tag': 'const char *', 'size': 1}]),
        'ovrKeyValuePair_makeString': ({'kind': 'Composite', 'tag': 'ovrKeyValuePair', 'size': 24, 'alignment': 8}, [{'kind': 'Ptr', 'tag': 'const char *', 'size': 1}] * 2),
    }
    for name, (result, arguments) in manual.items():
        lib = 'libovrplatformloader.so'
        if (lib, name) in wanted:
            source = 'CAPI.cs' if name == 'ovr_Log_NewEvent' else 'OVR_Types.h'
            rows[(lib, name)] = dict(library=lib, symbol=name, result=result, arguments=arguments, source='Pinned Oculus Platform SDK ' + source)
    emit(list(rows.values()), a.output / 'a32_vendor.cpp', 'vendor_descriptors')
    imports = list(csv.DictReader(a.imports.open()))
    lines = ['// Generated from First Contact version 7 imports.csv (APK dynsym census).']
    for r in imports:
        if r['importer'] not in ('libUE4.so', 'libOVRPlugin.so', 'libgnustl_shared.so'): continue
        lines.append('{' + ', '.join(json.dumps(r[k]) for k in ('importer', 'provider', 'symbol')) + '},')
    (a.output / 'a32_first_contact.inc').write_text('\n'.join(lines) + '\n')

if __name__ == '__main__': main()
