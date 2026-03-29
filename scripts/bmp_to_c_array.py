#!/usr/bin/env python3
"""Convert OpenCritter 32x32 BMP sprites to C header arrays."""
import struct
import os
import sys

def bmp_to_mono_bits(filepath):
    """Read a BMP and convert to 1-bit-per-pixel array (32x32 = 128 bytes)."""
    with open(filepath, 'rb') as f:
        data = f.read()

    # BMP header
    offset = struct.unpack_from('<I', data, 10)[0]
    width = struct.unpack_from('<i', data, 18)[0]
    height = struct.unpack_from('<i', data, 22)[0]
    bpp = struct.unpack_from('<H', data, 28)[0]

    if width != 32 or abs(height) != 32:
        print(f"  WARNING: {filepath} is {width}x{abs(height)}, expected 32x32", file=sys.stderr)
        return None

    # Determine if bottom-up (positive height) or top-down (negative)
    bottom_up = height > 0
    height = abs(height)

    # Read pixel data and convert to 1-bit
    rows = []
    if bpp == 1:
        # Already 1-bit
        row_bytes = (width + 7) // 8
        row_stride = (row_bytes + 3) & ~3  # BMP rows are 4-byte aligned
        for y in range(height):
            row_offset = offset + y * row_stride
            row = data[row_offset:row_offset + row_bytes]
            rows.append(row)
    elif bpp == 8:
        # 8-bit indexed — threshold at 128
        row_stride = (width + 3) & ~3
        for y in range(height):
            row_offset = offset + y * row_stride
            bits = []
            for x in range(width):
                pixel = data[row_offset + x]
                bits.append(1 if pixel < 128 else 0)
            # Pack bits into bytes
            row_bytes = bytearray(4)
            for x in range(32):
                if bits[x]:
                    row_bytes[x // 8] |= (0x80 >> (x % 8))
            rows.append(bytes(row_bytes))
    elif bpp == 24 or bpp == 32:
        # RGB/RGBA — threshold on brightness
        pixel_size = bpp // 8
        row_stride = (width * pixel_size + 3) & ~3
        for y in range(height):
            row_offset = offset + y * row_stride
            bits = []
            for x in range(width):
                px_off = row_offset + x * pixel_size
                b, g, r = data[px_off], data[px_off + 1], data[px_off + 2]
                brightness = (r + g + b) / 3
                bits.append(1 if brightness < 128 else 0)
            row_bytes = bytearray(4)
            for x in range(32):
                if bits[x]:
                    row_bytes[x // 8] |= (0x80 >> (x % 8))
            rows.append(bytes(row_bytes))
    else:
        print(f"  WARNING: unsupported bpp={bpp} in {filepath}", file=sys.stderr)
        return None

    if bottom_up:
        rows.reverse()

    # Flatten to 128 bytes
    result = b''
    for row in rows:
        result += row[:4]  # 4 bytes per row

    return result

def format_c_array(name, data):
    """Format byte data as a C array."""
    lines = [f"static const uint8_t {name}[128] = {{"]
    for row in range(32):
        offset = row * 4
        bytes_str = ', '.join(f'0x{b:02X}' for b in data[offset:offset+4])
        lines.append(f"    {bytes_str},")
    lines.append("};")
    return '\n'.join(lines)

def main():
    bmp_dir = '/home/mae/Documents/GitHub/OpenCritter/gfx/bitmaps/critters'
    output = '/home/mae/Documents/GitHub/KaSe_firmware/main/tama/tama_sprites.h'

    # Get all critter names
    files = sorted(os.listdir(bmp_dir))
    names = set()
    for f in files:
        if f.endswith('.bmp'):
            name = f.replace('_main.bmp', '').replace('_idle.bmp', '')
            names.add(name)
    names = sorted(names)

    header = [
        "/* Tamagotchi sprites — extracted from OpenCritter (MIT License).",
        "   https://github.com/SuperMechaCow/OpenCritter",
        "   32x32 monochrome bitmaps, 128 bytes each (MSB first). */",
        "#pragma once",
        "#include <stdint.h>",
        "",
        f"#define TAMA_SPRITE_W 32",
        f"#define TAMA_SPRITE_H 32",
        f"#define TAMA_SPRITE_BYTES 128",
        f"#define TAMA_CRITTER_COUNT {len(names)}",
        "",
    ]

    critter_names = []

    for name in names:
        main_path = os.path.join(bmp_dir, f'{name}_main.bmp')
        idle_path = os.path.join(bmp_dir, f'{name}_idle.bmp')

        if os.path.exists(main_path):
            data = bmp_to_mono_bits(main_path)
            if data:
                header.append(format_c_array(f'sprite_{name}_main', data))
                header.append("")

        if os.path.exists(idle_path):
            data = bmp_to_mono_bits(idle_path)
            if data:
                header.append(format_c_array(f'sprite_{name}_idle', data))
                header.append("")

        critter_names.append(name)

    # Critter lookup table
    header.append("/* Critter lookup: [index] = { main_sprite, idle_sprite, name } */")
    header.append("typedef struct { const uint8_t *main_frame; const uint8_t *idle_frame; const char *name; } tama_critter_t;")
    header.append("")
    header.append(f"static const tama_critter_t tama_critters[TAMA_CRITTER_COUNT] = {{")
    for name in critter_names:
        header.append(f'    {{ sprite_{name}_main, sprite_{name}_idle, "{name}" }},')
    header.append("};")

    with open(output, 'w') as f:
        f.write('\n'.join(header) + '\n')

    print(f"Generated {output} with {len(critter_names)} critters")

if __name__ == '__main__':
    main()
