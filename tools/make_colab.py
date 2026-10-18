"""Writes SonicManiaPSP.ipynb, the Google Colab notebook that turns a player's own Data.rsdk into a ready-to-copy
PSP folder (EBOOT.PBP built from this repository's source + Data.psp). Run it after changing the notebook's cells:

    python tools/make_colab.py
"""
import json
import os

REPO = 'https://github.com/ghurone/Sonic-Mania-PSP'
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'SonicManiaPSP.ipynb')

INTRO = '''# Sonic Mania PSP — converter

This notebook turns **your own copy** of Sonic Mania Plus into the files the PSP port needs, all in Google's cloud:
nothing to install on your computer.

**You need:** the `Data.rsdk` of Sonic Mania Plus (version 1.06, the Steam one). It is in the game's install folder,
e.g. `C:\\Program Files (x86)\\Steam\\steamapps\\common\\Sonic Mania\\Data.rsdk` (Steam → right-click the game →
*Manage* → *Browse local files*).

**How to use:**
1. Choose your settings in step ① below.
2. In the menu, click **Runtime → Run all** (or press `Ctrl+F9`).
3. If you chose to upload, click **Choose files** in step ② and pick your `Data.rsdk`.
4. Wait (about 15–25 minutes). At the end you get `SonicManiaPSP.zip`.
5. Extract the zip to the **root of your PSP's Memory Stick**: it creates `PSP/GAME/SonicMania/` with `EBOOT.PBP` and
   `Data.psp`.

The result contains your game's data: it is for your own use. **Do not share it.**
Port, source code and credits: [''' + REPO + '''](''' + REPO + ''')
'''

SETTINGS = '''#@title ① Settings { display-mode: "form" }
#@markdown **Where is your `Data.rsdk`?**
data_source = "Upload from my computer"  #@param ["Upload from my computer", "Google Drive"]
#@markdown If it is on Google Drive: its path inside *My Drive* (e.g. `Data.rsdk` or `Games/Data.rsdk`).
drive_path = "Data.rsdk"  #@param {type:"string"}

#@markdown ---
#@markdown **Extras added by the PSP port**
portuguese = True  #@param {type:"boolean"}
achievements_menu = True  #@param {type:"boolean"}

#@markdown ---
#@markdown **Where should the result go?** (Google Drive is faster and more reliable for the ~250 MB zip.)
result_to = "Download to my computer"  #@param ["Download to my computer", "Google Drive"]

print("Settings saved. Now click Runtime → Run all.")
'''

HELPERS = '''#@title (helpers) { display-mode: "form" }
import os, subprocess, shutil, glob, sys, time

WORK = "/content/smpsp"
LOGS = WORK + "/logs"
os.makedirs(LOGS, exist_ok=True)

class StepFailed(Exception):
    pass

def run(cmd, log, cwd=None, env=None, live=False):
    """Runs a shell command, keeping its output in a log file; shows it as it goes when live."""
    path = os.path.join(LOGS, log)
    with open(path, "a") as f:
        p = subprocess.Popen(cmd, shell=True, cwd=cwd, env=env, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True)
        for line in p.stdout:
            f.write(line)
            if live:
                print("   " + line.rstrip())
        p.wait()
    if p.returncode:
        print("\\n".join(open(path).read().splitlines()[-25:]))
        raise StepFailed("Something went wrong (log above, full log in %s). Please report it at %s/issues" % (path, "''' + REPO + '''"))

def step(text):
    print("▶ " + text, flush=True)
'''

GET_DATA = '''#@title ② Your Data.rsdk { display-mode: "form" }
DATA = WORK + "/Data.rsdk"
if data_source == "Google Drive":
    from google.colab import drive
    drive.mount("/content/drive")
    src = os.path.join("/content/drive/MyDrive", drive_path.strip().lstrip("/"))
    if not os.path.isfile(src):
        raise FileNotFoundError("No file at My Drive/%s — check the path in step ①." % drive_path)
    step("copying it from Google Drive...")
    shutil.copyfile(src, DATA)
else:
    from google.colab import files
    print("Click 'Choose files' and pick your Data.rsdk (about 200 MB: the upload takes a few minutes).")
    up = files.upload()
    if not up:
        raise RuntimeError("No file was uploaded.")
    name = next(iter(up))
    with open(DATA, "wb") as f:
        f.write(up[name])
    del up

with open(DATA, "rb") as f:
    head = f.read(6)
size = os.path.getsize(DATA) / 1e6
if head[:4] != b"RSDK":
    raise ValueError("This is not a Sonic Mania data pack (Data.rsdk). Pick the Data.rsdk from the game's folder.")
print("✔ Data.rsdk ok (%.0f MB)" % size)
'''

TOOLS = '''#@title ③ Install the tools (about 3 minutes) { display-mode: "form" }
PSPDEV_TAG = "v20261001"      # the PSP toolchain (pspdev)
PSPDEV_OLD = "v20241101"      # same, built on an older Linux, for older Colab machines
ATRACDENC = "0.2.3"           # the ATRAC3 music encoder

step("downloading the port's source code...")
if not os.path.isdir(WORK + "/repo"):
    run("git clone -q --depth 1 ''' + REPO + ''' repo", "tools.log", cwd=WORK)

step("installing build packages...")
run("apt-get -qq update && apt-get -qq install -y cmake libsndfile1-dev xz-utils libreadline8 libusb-0.1-4 libarchive-tools > /dev/null", "tools.log")
run("pip -q install lz4 numpy pillow", "tools.log")

step("installing the PSP toolchain (pspdev)...")
def pspdev_works():
    return subprocess.run("%s/pspdev/bin/psp-gcc --version" % WORK, shell=True, capture_output=True).returncode == 0
for tag in (PSPDEV_TAG, PSPDEV_OLD):
    if pspdev_works():
        break
    shutil.rmtree(WORK + "/pspdev", ignore_errors=True)
    run("wget -q https://github.com/pspdev/pspdev/releases/download/%s/pspdev-ubuntu-latest-x86_64.tar.gz -O pspdev.tar.gz"
        " && tar xzf pspdev.tar.gz && rm pspdev.tar.gz" % tag, "tools.log", cwd=WORK)
if not pspdev_works():
    raise StepFailed("The PSP toolchain does not run on this machine. Please report it at ''' + REPO + '''/issues")

step("building the ATRAC3 encoder (atracdenc)...")
if not os.path.exists(WORK + "/atracdenc-bin"):
    run("wget -q https://github.com/dcherednik/atracdenc/releases/download/%s/atracdenc-%s.tar.xz -O a.tar.xz && tar xJf a.tar.xz"
        % (ATRACDENC, ATRACDENC), "tools.log", cwd=WORK)
    src = glob.glob(WORK + "/atracdenc-%s*" % ATRACDENC)[0]
    run("cmake -S . -B build -DCMAKE_BUILD_TYPE=Release > /dev/null && cmake --build build -j%d > /dev/null" % os.cpu_count(), "tools.log", cwd=src)
    shutil.copy(next(p for p in glob.glob(src + "/build/**/atracdenc", recursive=True) if os.path.isfile(p)), WORK + "/atracdenc-bin")

step("checking ffmpeg...")
def ffmpeg_ok():
    out = subprocess.run("ffmpeg -hide_banner -h full", shell=True, capture_output=True, text=True).stdout
    return "fps_mode" in out and shutil.which("ffprobe")
if not ffmpeg_ok():
    run("wget -q https://github.com/BtbN/FFmpeg-Builds/releases/download/latest/ffmpeg-master-latest-linux64-gpl.tar.xz -O f.tar.xz"
        " && tar xJf f.tar.xz && rm f.tar.xz", "tools.log", cwd=WORK)
    os.environ["PATH"] = glob.glob(WORK + "/ffmpeg-*linux64*/bin")[0] + ":" + os.environ["PATH"]
if not ffmpeg_ok():
    raise StepFailed("Could not set up ffmpeg. Please report it at ''' + REPO + '''/issues")
os.environ["PSPDEV"] = WORK + "/pspdev"
os.environ["PATH"] = WORK + "/pspdev/bin:" + os.environ["PATH"]
print("✔ tools ready")
'''

CONVERT = '''#@title ④ Convert the game (about 10–20 minutes) { display-mode: "form" }
step("converting Data.rsdk into Data.psp (music, videos, images)...")
extra = ("" if portuguese else " --no-portuguese") + ("" if achievements_menu else " --no-achievements")
run("python tools/build_psp_data.py %s %s/Data.psp --atracdenc %s/atracdenc-bin --music-cache %s/music%s"
    % (DATA, WORK, WORK, WORK, extra), "convert.log", cwd=WORK + "/repo", live=True)
print("✔ Data.psp ready (%.0f MB)" % (os.path.getsize(WORK + "/Data.psp") / 1e6))
'''

BUILD = '''#@title ⑤ Build the game for the PSP (about 5 minutes) { display-mode: "form" }
step("compiling EBOOT.PBP from the source code...")
run("make -j%d" % os.cpu_count(), "build.log", cwd=WORK + "/repo/psp")
print("✔ EBOOT.PBP ready (%.1f MB)" % (os.path.getsize(WORK + "/repo/psp/EBOOT.PBP") / 1e6))
'''

DELIVER = '''#@title ⑥ Get the result { display-mode: "form" }
step("packing PSP/GAME/SonicMania/...")
pack = WORK + "/out"
shutil.rmtree(pack, ignore_errors=True)
game = pack + "/PSP/GAME/SonicMania"
os.makedirs(game)
shutil.copy(WORK + "/repo/psp/EBOOT.PBP", game)
shutil.move(WORK + "/Data.psp", game + "/Data.psp")
ZIP = "/content/SonicManiaPSP.zip"
if os.path.exists(ZIP):
    os.remove(ZIP)
run("zip -q -0 -r %s PSP" % ZIP, "deliver.log", cwd=pack)  # (no compression: the data is compressed already)
shutil.move(game + "/Data.psp", WORK + "/Data.psp")

if result_to == "Google Drive":
    from google.colab import drive
    if not os.path.isdir("/content/drive/MyDrive"):
        drive.mount("/content/drive")
    shutil.copyfile(ZIP, "/content/drive/MyDrive/SonicManiaPSP.zip")
    print("✔ Saved to your Google Drive: My Drive/SonicManiaPSP.zip")
else:
    from google.colab import files
    print("✔ Downloading SonicManiaPSP.zip (about 250 MB)... If it does not start, open the 📁 panel on the left,")
    print("  right-click SonicManiaPSP.zip and choose Download.")
    files.download(ZIP)

print()
print("Extract SonicManiaPSP.zip to the root of your PSP's Memory Stick (it creates PSP/GAME/SonicMania/), then start")
print("Sonic Mania from the PSP's Game menu. Have fun!")
'''


def cell(kind, text):
    lines = text.strip('\n').split('\n')
    src = [l + '\n' for l in lines[:-1]] + [lines[-1]]
    if kind == 'markdown':
        return {'cell_type': 'markdown', 'metadata': {}, 'source': src}
    return {'cell_type': 'code', 'execution_count': None, 'metadata': {'cellView': 'form'}, 'outputs': [], 'source': src}


notebook = {
    'nbformat': 4,
    'nbformat_minor': 0,
    'metadata': {'colab': {'name': 'SonicManiaPSP.ipynb', 'provenance': []},
                 'kernelspec': {'name': 'python3', 'display_name': 'Python 3'}, 'language_info': {'name': 'python'}},
    'cells': [cell('markdown', INTRO), cell('code', SETTINGS), cell('code', HELPERS), cell('code', GET_DATA),
              cell('code', TOOLS), cell('code', CONVERT), cell('code', BUILD), cell('code', DELIVER)],
}
with open(OUT, 'w', encoding='utf-8', newline='\n') as f:
    json.dump(notebook, f, ensure_ascii=False, indent=1)
    f.write('\n')
print('wrote', os.path.normpath(OUT))
