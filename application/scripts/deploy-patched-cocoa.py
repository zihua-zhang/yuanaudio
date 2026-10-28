#!/usr/bin/env python3
"""Deploy a hash-pinned Qt 6.11.2 Cocoa plugin before the normal signing gate."""
from pathlib import Path
import hashlib,json,shutil,subprocess,sys
source,expected,app,qt=sys.argv[1:]
source=Path(source);app=Path(app);qt=Path(qt)
if hashlib.sha256(source.read_bytes()).hexdigest()!=expected:
 raise SystemExit("Patched plugin hash mismatch")
version=subprocess.check_output([str(qt/'bin/qmake'),'-query','QT_VERSION'],text=True).strip()
if version!='6.11.2':
 raise SystemExit('Exact Qt private ABI version is required')
subprocess.run(['lipo','-verify_arch','arm64',str(source)],check=True)
target=app/'Contents/PlugIns/platforms/libqcocoa.dylib'
if not target.parent.is_dir() or target.is_symlink():
 raise SystemExit('Unexpected Cocoa deployment path')
shutil.copy2(source,target)
load=subprocess.check_output(['otool','-l',str(target)],text=True).splitlines()
rpaths=[]
for i,line in enumerate(load):
 if line.strip()=='cmd LC_RPATH':
  for x in load[i+1:i+5]:
   if x.strip().startswith('path '):rpaths.append(x.strip().split(' (offset')[0][5:]);break
for r in rpaths:
 subprocess.run(['install_name_tool','-delete_rpath',r,str(target)],check=True)
subprocess.run(['install_name_tool','-add_rpath','@loader_path/../../Frameworks',str(target)],check=True)
legal=app/'Contents/Resources/Legal/Qt-Cocoa-8148c48';legal.mkdir(parents=True,exist_ok=True)
repair=Path(__file__).resolve().parents[2]
hand=repair/'handoff/源音效管理工具-Mac闪退修复资料-0.12.0/qt-upstream'
for name in ['NOTICE.md','qt-8148c48.patch']:shutil.copy2(hand/name,legal/name)
shutil.copytree(hand/'licenses',legal/'licenses',dirs_exist_ok=True)
for name in ['qcocoaaccessibilityelement.h','qcocoaaccessibilityelement.mm']:
 shutil.copy2(repair/'sources/qtbase-6.11.2/src/plugins/platforms/cocoa'/name,legal/name)
record={'qt_version':version,'upstream_commit':'8148c48deb8e4345bacb797cb75c011df6f797fa','source_plugin_sha256':expected,'deployed_plugin_sha256_before_signing':hashlib.sha256(target.read_bytes()).hexdigest(),'uuid':subprocess.check_output(['dwarfdump','--uuid',str(target)],text=True),'target':str(target)}
(app.parent/'macos-fixed-cocoa-deployment.json').write_text(json.dumps(record,ensure_ascii=False,indent=2))
print('Verified patched Cocoa deployment:',json.dumps(record,ensure_ascii=False))
