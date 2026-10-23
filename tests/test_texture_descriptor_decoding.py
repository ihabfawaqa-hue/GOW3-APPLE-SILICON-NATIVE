"""Mirror the strict 2D GCN image-descriptor decoder used by the Metal path."""

from pathlib import Path
import struct

ASSET_GNF = Path("/Volumes/1TB/gow3_cusa01623_extracted/EXEC/INTRO/BOOT0_00.GNF")


def decode_texture_descriptor(dw, num_words=8, mem_size=0x200000000):
    if dw is None or num_words < 5 or len(dw) < 5:
        return None

    width = (dw[2] & 0x3FFF) + 1
    height = ((dw[2] >> 14) & 0x3FFF) + 1
    if not (4 <= width <= 8192 and 4 <= height <= 8192):
        return None

    dfmt = (dw[1] >> 20) & 0x3F
    nfmt = (dw[1] >> 26) & 0xF
    image_type = (dw[3] >> 28) & 0xF
    if image_type != 9:  # Color2D only
        return None

    formats = {
        (10, 0): ("RGBA8Unorm", 0),
        (10, 9): ("RGBA8Unorm_sRGB", 0),
        (35, 0): ("BC1_RGBA", 8),
        (35, 9): ("BC1_RGBA_sRGB", 8),
        (36, 0): ("BC2_RGBA", 16),
        (36, 9): ("BC2_RGBA_sRGB", 16),
        (37, 0): ("BC3_RGBA", 16),
        (37, 9): ("BC3_RGBA_sRGB", 16),
        (38, 0): ("BC4_RUnorm", 8),
        (39, 0): ("BC5_RGUnorm", 16),
        (41, 0): ("BC7_RGBAUnorm", 16),
        (41, 9): ("BC7_RGBAUnorm_sRGB", 16),
    }
    fmt = formats.get((dfmt, nfmt))
    if fmt is None:
        return None
    format_name, bytes_per_block = fmt

    pitch = ((dw[4] >> 13) & 0x3FFF) + 1
    if pitch < width or pitch > 16384:
        return None

    shifted = (dw[0] << 8) & 0xFFFFFFFFFFFFFFFF
    shifted_full = (shifted | ((dw[1] & 0x3F) << 40)) & 0xFFFFFFFFFFFFFFFF
    direct = dw[0] & 0xFFFFFFFF
    combined = (((dw[1] & 0x3F) << 32) | dw[0]) & 0xFFFFFFFFFFFFFFFF
    address = next((candidate for candidate in (shifted_full, shifted, direct, combined)
                    if 0x10000 <= candidate < mem_size), 0)
    if address == 0:
        return None

    if bytes_per_block:
        bytes_per_row = ((pitch + 3) // 4) * bytes_per_block
        total_bytes = bytes_per_row * ((height + 3) // 4)
    else:
        bytes_per_row = pitch * 4
        total_bytes = bytes_per_row * height

    return {
        "addr": address,
        "width": width,
        "height": height,
        "dfmt": dfmt,
        "nfmt": nfmt,
        "image_type": image_type,
        "format": format_name,
        "pitch": pitch,
        "bytes_per_row": bytes_per_row,
        "total_bytes": total_bytes,
    }


def main():
    # The mounted game's actual GNF starts with a zero base followed by these
    # descriptor words. The runtime fixture below relocates its base to 0xbbe08000.
    raw_gnf_words = [0x00000000, 0x02600008, 0x010DC77F, 0x90D00324, 0x00EFE000]
    if ASSET_GNF.is_file():
        data = ASSET_GNF.read_bytes()
        assert len(data) >= 36 and data[:4] == b"GNF "
        observed = struct.unpack("<5I", data[16:36])
        assert list(observed) == raw_gnf_words, f"unexpected BOOT0_00.GNF descriptor: {observed!r}"
        print("[+] Mounted BOOT0_00.GNF header descriptor verified byte-for-byte")
    else:
        print("[i] Game volume not mounted; using the verified descriptor fixture")

    relocated = [0x00BBE080, 0x02600000, *raw_gnf_words[2:], 0, 0, 0]
    result = decode_texture_descriptor(relocated)
    assert result is not None
    assert result["addr"] == 0xBBE08000
    assert (result["width"], result["height"], result["pitch"]) == (1920, 1080, 1920)
    assert (result["dfmt"], result["nfmt"], result["image_type"]) == (38, 0, 9)
    assert result["format"] == "BC4_RUnorm"
    assert (result["bytes_per_row"], result["total_bytes"]) == (3840, 1036800)
    print("[+] Relocated GNF descriptor: 1920x1080 BC4_RUnorm, 3840-byte rows")

    def descriptor(address=0xBC600000, data_format=37, number_format=0,
                   image_type=9, pitch=1920, width=1920, height=1080):
        dw0 = (address >> 8) & 0xFFFFFFFF
        dw1 = ((data_format & 0x3F) << 20) | ((number_format & 0xF) << 26)
        dims = ((height - 1) << 14) | (width - 1)
        dw3 = (image_type & 0xF) << 28
        dw4 = (pitch - 1) << 13
        return [dw0, dw1, dims, dw3, dw4, 0, 0, 0]

    cases = [
        (35, 0, "BC1_RGBA", 8), (36, 0, "BC2_RGBA", 16),
        (37, 0, "BC3_RGBA", 16), (38, 0, "BC4_RUnorm", 8),
        (39, 0, "BC5_RGUnorm", 16), (41, 0, "BC7_RGBAUnorm", 16),
        (10, 0, "RGBA8Unorm", 0), (41, 9, "BC7_RGBAUnorm_sRGB", 16),
    ]
    for data_format, number_format, expected, block_bytes in cases:
        decoded = decode_texture_descriptor(descriptor(data_format=data_format, number_format=number_format))
        assert decoded and decoded["format"] == expected, (data_format, number_format, decoded)
        if block_bytes:
            assert decoded["bytes_per_row"] == 480 * block_bytes
    print(f"[+] Format table: {len(cases)} supported combinations passed")

    assert decode_texture_descriptor(descriptor(data_format=0)) is None
    assert decode_texture_descriptor(descriptor(image_type=13)) is None
    assert decode_texture_descriptor(descriptor(pitch=1919)) is None
    assert decode_texture_descriptor(descriptor(number_format=1)) is None
    assert decode_texture_descriptor(descriptor(), num_words=4) is None
    assert decode_texture_descriptor([0, 0x02600001, 0x010DC77F, 0x90000000, 0x00EFE000],
                                      mem_size=0x20000000000)["addr"] == 0x10000000000
    print("[+] Invalid format/type/pitch/length rejection and 38-bit address extraction passed")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
