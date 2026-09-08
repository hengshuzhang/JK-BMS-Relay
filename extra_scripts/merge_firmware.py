Import("env")

from pathlib import Path
import subprocess
import sys

BUILD_DIR = Path(env.subst("$BUILD_DIR"))


def _find_boot_app0():
    framework_dir = env.PioPlatform().get_package_dir("framework-arduinoespressif32")
    if framework_dir:
        path = Path(framework_dir) / "tools" / "partitions" / "boot_app0.bin"
        if path.is_file():
            return path
    raise FileNotFoundError(
        "Arduino-ESP32 boot_app0.bin was not found in the framework package"
    )


def _required(name):
    path = BUILD_DIR / name
    if not path.is_file():
        raise FileNotFoundError("Required firmware image is missing: %s" % path)
    return path


def _esptool_command():
    python = env.subst("$PYTHONEXE") or sys.executable
    tool_dir = env.PioPlatform().get_package_dir("tool-esptoolpy")
    if tool_dir:
        script = Path(tool_dir) / "esptool.py"
        if script.is_file():
            return [python, str(script)], "merge_bin", True
    return [python, "-m", "esptool"], "merge-bin", False


def merge_firmware(source, target, env):
    output = BUILD_DIR / "firmware_merged.bin"
    boot_app0 = _find_boot_app0()
    prefix, merge_command, legacy = _esptool_command()
    if legacy:
        options = [
            "--flash_mode",
            "keep",
            "--flash_freq",
            "keep",
            "--flash_size",
            "4MB",
            "-o",
            str(output),
        ]
    else:
        options = [
            "--format",
            "raw",
            "--flash-mode",
            "keep",
            "--flash-freq",
            "keep",
            "--flash-size",
            "4MB",
            "--output",
            str(output),
        ]
    command = prefix + ["--chip", "esp32s3", merge_command] + options + [
        "0x0000",
        str(_required("bootloader.bin")),
        "0x8000",
        str(_required("partitions.bin")),
        "0xE000",
        str(boot_app0),
        "0x20000",
        str(_required("firmware.bin")),
    ]
    print("Generating %s for Espressif Flash Download Tool" % output)
    subprocess.check_call(command)

    flash_size = 4 * 1024 * 1024
    current_size = output.stat().st_size
    if current_size > flash_size:
        raise RuntimeError("Merged firmware is larger than the configured 4 MB flash")
    with output.open("ab") as merged:
        remaining = flash_size - current_size
        while remaining:
            chunk = min(remaining, 64 * 1024)
            merged.write(b"\xff" * chunk)
            remaining -= chunk


env.AddPostAction("$BUILD_DIR/firmware.bin", merge_firmware)
