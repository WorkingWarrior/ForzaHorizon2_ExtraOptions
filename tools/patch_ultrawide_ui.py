"""Center FH2's 1920x1080 XAML interface on a 3440x1440 display.

The game uses a nonstandard ZIP extra field (0x1123) in the central directory.
It stores the absolute offset of each entry's compressed data. Standard ZIP
writers put that field into local headers too, which makes this archive
unreadable to the game. This script preserves the original local records and
only recompresses the changed XAML entries.
"""

import argparse
import hashlib
import io
import os
from pathlib import Path
import re
import shutil
import stat
import struct
import tempfile
import xml.etree.ElementTree as ET
from zipfile import ZipFile
import zlib


ORIGINAL_SHA256 = "33ecf0453f0d18f20408317c2bab7f0413be6e08e2b8188398c69930f8dcfa34"
PATCHED_SHA256 = "22200c68da0ae3d81d635d04ec8f57687c0c4f07873277758e9180fee4abc1eb"
SCENE_SCALE_X = "0.744186046511628"  # (1920 * 1440 / 1080) / 3440
BLUR_WIDTH = b"2580"  # 3440 physical pixels at the UI's 1440/1080 scale
EXTRA_TAG = b"\x23\x11\x04\x00"
FIXED_UI_FILES = (
    "AnthemApp.xaml",
    "App.xaml",
    "Resources/Anthem/Global_Material.xaml",
    "Resources/Anthem/Shared_Bars.xaml",
    "Resources/Anthem/Shared_LoadingScreens.xaml",
    "Scenes/Anthem/A_Hud.xaml",
    "Scenes/Anthem/A_Splash.xaml",
    "Scenes/Anthem/Loading.xaml",
)
EXPECTED_BLUR_SCENES = 25


def set_tag_attribute(tag, element, attribute, value):
    pattern = rb"\b" + attribute + rb'="[^"]*"'
    replacement = attribute + b'="' + value + b'"'
    if re.search(pattern, tag):
        return re.sub(pattern, replacement, tag, count=1)
    return tag.replace(element, element + b" " + replacement, 1)


def expand_screen_blur(data, name):
    blur_count = 0

    def material(match):
        nonlocal blur_count
        tag = match.group()
        if b"A_FZMaterialPresetGlass_ScreenBlur" not in tag:
            return tag
        blur_count += 1
        tag = set_tag_attribute(tag, b"<local:FZMaterial", b"Width", BLUR_WIDTH)
        return set_tag_attribute(tag, b"<local:FZMaterial", b"HorizontalAlignment", b"Center")

    result = re.sub(rb"<local:FZMaterial\b[^>]*>", material, data)
    if blur_count != 1:
        raise ValueError(f"Expected one screen blur in {name}; found {blur_count}")

    def capture(match):
        tag = match.group()
        tag = set_tag_attribute(tag, b"<local:FZMaterialGlassCapture", b"Width", BLUR_WIDTH)
        return set_tag_attribute(tag, b"<local:FZMaterialGlassCapture", b"HorizontalAlignment", b"Center")

    result = re.sub(rb"<local:FZMaterialGlassCapture\b[^>]*>", capture, result)
    if name == "Scenes/Anthem/A_PauseMenu.xaml":
        content = b'Style="{StaticResource A_Style_ContentControl_ScreenTilt}"'
        if result.count(content) != 1:
            raise ValueError("Pause menu content control was not found exactly once")
        result = result.replace(content, content + b' ClipToBounds="True"')
    return result


def digest(data):
    return hashlib.sha256(data).hexdigest()


def modified_xaml(name, data):
    if name in ("App.xaml", "AnthemApp.xaml"):
        bom = b"\xef\xbb\xbf" if data.startswith(b"\xef\xbb\xbf") else b""
        source = data.decode("utf-8-sig")
        setters = (
            '<Setter Property="RenderTransformOrigin" Value="0.5,0.5" />\n'
            '                <Setter Property="RenderTransform"><Setter.Value>'
            f'<ScaleTransform ScaleX="{SCENE_SCALE_X}" ScaleY="1" />'
            '</Setter.Value></Setter>\n'
        )
        pattern = r'(<Style x:Key="A_Style_Page_Generic"[^>]*>)(.*?)(</Style>)'
        source, count = re.subn(
            pattern,
            lambda match: match.group(1) + match.group(2) + "                " + setters + match.group(3),
            source,
            flags=re.S,
        )
        if count != 1:
            raise ValueError(f"Expected one generic page style in {name}; found {count}")
        result = bom + source.encode("utf-8")
    elif name == "Scenes/Anthem/A_Hud.xaml":
        result = data.replace(
            b'Style="{StaticResource A_Style_ScreenSize}"',
            b'Style="{StaticResource A_Style_Page_Generic}"',
            1,
        )
        if result == data:
            raise ValueError("HUD page style was not found")
    elif name == "Resources/Anthem/Global_Material.xaml":
        style = re.search(
            rb'<Style x:Key="A_Style_FZMaterial_Glass_ScreenBackground".*?</Style>',
            data,
            flags=re.S,
        )
        if style is None or style.group().count(b'<Setter Property="Width" Value="1920" />') != 1:
            raise ValueError("Screen background material style was not found")
        replacement = style.group().replace(
            b'<Setter Property="Width" Value="1920" />',
            b'<Setter Property="Width" Value="2580" />\n'
            b'        <Setter Property="HorizontalAlignment" Value="Center" />',
        )
        result = data[: style.start()] + replacement + data[style.end() :]
    elif name == "Resources/Anthem/Shared_Bars.xaml":
        style = re.search(rb'<Style x:Key="A_Style_PauseMenuBlur".*?</Style>', data, flags=re.S)
        if style is None or style.group().count(b'<Setter Property="Preset"') != 1:
            raise ValueError("Shared pause menu blur style was not found")
        replacement = style.group().replace(
            b'</Style>',
            b'    <Setter Property="Width" Value="2580" />\n'
            b'    <Setter Property="HorizontalAlignment" Value="Center" />\n'
            b'\t</Style>',
        )
        result = data[: style.start()] + replacement + data[style.end() :]
    elif name == "Resources/Anthem/Shared_LoadingScreens.xaml":
        result = data
        for key in (
            b"A_Style_FZMaterial_LoadingBackground",
            b"A_Style_FZMaterial_StaticLoadingBackground",
        ):
            style = re.search(rb'<Style x:Key="' + key + rb'".*?</Style>', result, flags=re.S)
            if style is None or style.group().count(b'<Setter Property="Width" Value="1920" />') != 1:
                raise ValueError(f"Loading background style {key.decode()} was not found")
            replacement = style.group().replace(
                b'<Setter Property="Width" Value="1920" />',
                b'<Setter Property="Width" Value="2580" />\n'
                b'            <Setter Property="Stretch" Value="UniformToFill" />',
            )
            result = result[: style.start()] + replacement + result[style.end() :]
    elif name == "Scenes/Anthem/A_Splash.xaml":
        media = b'<MediaElement x:Name="Video" Style="{StaticResource A_Style_ScreenSize}" />'
        if data.count(media) != 1:
            raise ValueError("Splash video was not found exactly once")
        result = data.replace(
            media,
            b'<MediaElement x:Name="Video" Style="{StaticResource A_Style_ScreenSize}" '
            b'Width="2580" Height="1080" HorizontalAlignment="Center" Stretch="UniformToFill" />',
        )
    elif name == "Scenes/Anthem/Loading.xaml":
        background = b'<local:FZMaterial x:Name="A_CyclingLoadingBackgrounds"'
        if data.count(background) != 1:
            raise ValueError("Loading background was not found exactly once")
        result = data.replace(
            background,
            b'<Border Width="2580" Height="1080" HorizontalAlignment="Center" '
            b'Background="Black" />\n        ' + background,
        )
    elif name.startswith("Scenes/Anthem/") and name.endswith(".xaml"):
        result = expand_screen_blur(data, name)
    else:
        raise ValueError(f"Unexpected XAML entry: {name}")
    ET.fromstring(result)
    return result


def build_archive(original, changes):
    """Preserve original ZIP records; adjust only changed data and offsets."""
    with ZipFile(io.BytesIO(original)) as archive:
        entries = archive.infolist()
        eocd_offset = original.rfind(b"PK\x05\x06")
        if eocd_offset < 0 or eocd_offset + 22 > len(original):
            raise ValueError("ZIP end record is missing")
        comment_size = struct.unpack_from("<H", original, eocd_offset + 20)[0]
        if eocd_offset + 22 + comment_size != len(original):
            raise ValueError("ZIP end record has trailing data")
        central_size, central_offset = struct.unpack_from("<II", original, eocd_offset + 12)
        if central_offset + central_size != eocd_offset:
            raise ValueError("ZIP directory bounds do not match")

        central_records = []
        cursor = central_offset
        for entry in entries:
            if original[cursor : cursor + 4] != b"PK\x01\x02":
                raise ValueError(f"Missing central record for {entry.filename}")
            name_size, extra_size, comment_size = struct.unpack_from("<HHH", original, cursor + 28)
            size = 46 + name_size + extra_size + comment_size
            record = original[cursor : cursor + size]
            if (
                name_size != len(entry.filename.encode("utf-8"))
                or extra_size != 8
                or comment_size != 0
                or record[46 + name_size : 50 + name_size] != EXTRA_TAG
            ):
                raise ValueError(f"Unexpected central record for {entry.filename}")
            central_records.append(record)
            cursor += size
        if cursor != eocd_offset:
            raise ValueError("ZIP directory length does not match")

        output = bytearray()
        offsets = []
        for entry, central in zip(entries, central_records):
            offset = entry.header_offset
            if original[offset : offset + 4] != b"PK\x03\x04":
                raise ValueError(f"Missing local record for {entry.filename}")
            name_size, extra_size = struct.unpack_from("<HH", original, offset + 26)
            if extra_size != 0 or name_size != len(entry.filename.encode("utf-8")):
                raise ValueError(f"Unexpected local record for {entry.filename}")
            data_offset = offset + 30 + name_size
            if struct.unpack_from("<I", central, 50 + name_size)[0] != data_offset:
                raise ValueError(f"Invalid data offset for {entry.filename}")
            header = bytearray(original[offset:data_offset])
            compressed = original[data_offset : data_offset + entry.compress_size]
            if entry.filename in changes:
                data = changes[entry.filename]
                compressor = zlib.compressobj(level=6, wbits=-15)
                compressed = compressor.compress(data) + compressor.flush()
                crc, uncompressed_size = zlib.crc32(data), len(data)
                struct.pack_into("<III", header, 14, crc, len(compressed), uncompressed_size)
            else:
                crc, uncompressed_size = entry.CRC, entry.file_size
            header_offset = len(output)
            output += header
            new_data_offset = len(output)
            output += compressed
            offsets.append((header_offset, new_data_offset, crc, len(compressed), uncompressed_size))

        new_central_offset = len(output)
        for entry, old_record, values in zip(entries, central_records, offsets):
            header_offset, data_offset, crc, compressed_size, uncompressed_size = values
            record = bytearray(old_record)
            struct.pack_into("<III", record, 16, crc, compressed_size, uncompressed_size)
            struct.pack_into("<I", record, 42, header_offset)
            name_size = len(entry.filename.encode("utf-8"))
            struct.pack_into("<I", record, 50 + name_size, data_offset)
            output += record
        end_record = bytearray(original[eocd_offset:])
        struct.pack_into("<II", end_record, 12, len(output) - new_central_offset, new_central_offset)
        output += end_record

        with ZipFile(io.BytesIO(output)) as result:
            if result.testzip() is not None or result.namelist() != archive.namelist():
                raise ValueError("Patched ZIP failed integrity validation")
            for entry in result.infolist():
                offset = entry.header_offset
                name_size, extra_size = struct.unpack_from("<HH", output, offset + 26)
                if extra_size != 0 or struct.unpack_from("<I", entry.extra, 4)[0] != offset + 30 + name_size:
                    raise ValueError(f"Invalid patched offset for {entry.filename}")
                expected = changes.get(entry.filename)
                if expected is None:
                    expected = archive.read(entry.filename)
                if result.read(entry.filename) != expected:
                    raise ValueError(f"Patched content mismatch for {entry.filename}")
        return bytes(output)


def atomic_write(path, data, mode):
    with tempfile.NamedTemporaryFile(dir=path.parent, prefix=path.name + ".", delete=False) as temporary:
        temporary_path = Path(temporary.name)
        try:
            temporary.write(data)
            temporary.flush()
            os.fsync(temporary.fileno())
            os.chmod(temporary_path, mode)
            os.replace(temporary_path, path)
        finally:
            temporary_path.unlink(missing_ok=True)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("action", choices=("check", "apply", "restore"))
    parser.add_argument("game_root", type=Path, help="Folder containing Mount/media/UI.zip")
    arguments = parser.parse_args()
    ui_zip = arguments.game_root / "Mount/media/UI.zip"
    backup = ui_zip.with_name("UI.zip.fh2-ultrawide-original")
    current = ui_zip.read_bytes()
    current_hash = digest(current)
    backup_hash = digest(backup.read_bytes()) if backup.exists() else None

    if arguments.action == "check":
        state = "original" if current_hash == ORIGINAL_SHA256 else "patched" if current_hash == PATCHED_SHA256 else "unsupported"
        print(f"UI.zip: {state} ({current_hash})")
        print(f"Backup: {backup_hash or 'absent'}")
        return

    if arguments.action == "apply":
        if current_hash == PATCHED_SHA256:
            if backup_hash != ORIGINAL_SHA256:
                raise ValueError("UI is patched but the original backup is missing or invalid")
            print("UI is already patched")
            return
        if current_hash != ORIGINAL_SHA256:
            raise ValueError("Unsupported UI.zip; no files changed")
        if backup_hash not in (None, ORIGINAL_SHA256):
            raise ValueError("An unrelated backup exists; no files changed")
        if build_archive(current, {}) != current:
            raise ValueError("The archive cannot be rebuilt exactly; no files changed")
        with ZipFile(io.BytesIO(current)) as archive:
            blur_scenes = tuple(
                name for name in archive.namelist()
                if name.startswith("Scenes/Anthem/")
                and name.endswith(".xaml")
                and b"A_FZMaterialPresetGlass_ScreenBlur" in archive.read(name)
            )
            if len(blur_scenes) != EXPECTED_BLUR_SCENES:
                raise ValueError(f"Expected {EXPECTED_BLUR_SCENES} screen blur scenes; found {len(blur_scenes)}")
            changes = {
                name: modified_xaml(name, archive.read(name))
                for name in FIXED_UI_FILES + blur_scenes
            }
        patched = build_archive(current, changes)
        if digest(patched) != PATCHED_SHA256:
            raise ValueError("Unexpected patched archive hash; no files changed")
        if backup_hash is None:
            with backup.open("xb") as output:
                output.write(current)
            shutil.copymode(ui_zip, backup)
        atomic_write(ui_zip, patched, stat.S_IMODE(ui_zip.stat().st_mode))
        print(f"Patched {ui_zip}; original saved at {backup}")
        return

    if current_hash == ORIGINAL_SHA256:
        print("UI is already original")
        return
    if current_hash != PATCHED_SHA256 or backup_hash != ORIGINAL_SHA256:
        raise ValueError("UI or backup hash is unexpected; no files changed")
    atomic_write(ui_zip, backup.read_bytes(), stat.S_IMODE(ui_zip.stat().st_mode))
    print(f"Restored {ui_zip}")


if __name__ == "__main__":
    main()
