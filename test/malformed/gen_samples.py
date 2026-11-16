#!/usr/bin/env python3
"""Generate the malformed/valid glTF 2.0 samples for the cgltf boundary-check
regression matrix.

Each sample is a hand-crafted GLB (or .gltf + buffer) with a tiny JSON chunk
and a small binary payload. The malformed samples intentionally declare
out-of-range bufferViews, illegal byteStride values, misaligned offsets and
overflowing sizes; the parser must reject them with a cgltf error instead of
reading out of bounds. The valid samples must keep parsing successfully.

Run with: python3 -I gen_samples.py <out_dir>
Writes the samples plus manifest.txt with lines "<file> <ok|error>".
"""
import base64
import json
import os
import struct
import sys


def pad4(b: bytes, pad: bytes = b" ") -> bytes:
    r = len(b) % 4
    return b + pad * ((4 - r) % 4)


def glb(json_obj, bin_blob: bytes = b"") -> bytes:
    payload = json.dumps(json_obj, separators=(",", ":")).encode("ascii")
    payload = pad4(payload, b" ")
    json_chunk = struct.pack("<I", len(payload)) + b"JSON" + payload
    out = json_chunk
    if bin_blob:
        bin_blob = pad4(bin_blob, b"\x00")
        out += struct.pack("<I", len(bin_blob)) + b"BIN\x00" + bin_blob
    total = 12 + len(out)
    return struct.pack("<III", 0x46546C67, 2, total) + out


def gltf_text(json_obj) -> str:
    return json.dumps(json_obj, separators=(",", ":"))


# A buffer whose declared byteLength matches the embedded BIN chunk size.
BIN8 = bytes(range(8))


def base_doc(buffer_views, accessors, meshes=None, buffer_byte_length=8):
    doc = {
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": buffer_byte_length}],
        "bufferViews": buffer_views,
        "accessors": accessors,
    }
    if meshes is not None:
        doc["meshes"] = meshes
    return doc


def samples():
    # name -> (expectation, kind, payload)
    # kind "glb": payload is the GLB blob; kind "gltf": payload is JSON text.
    out = {}

    def add(name, expect, blob):
        out[name] = (expect, blob)

    # --- Out-of-bounds bufferViews -------------------------------------

    # Sparse indices read through a view whose byteLength exceeds the buffer.
    add("view_oob_sparse_indices.glb", "error", glb(base_doc(
        [
            {"buffer": 0, "byteOffset": 0, "byteLength": 8},
            {"buffer": 0, "byteOffset": 0, "byteLength": 100000},
        ],
        [
            {
                "bufferView": 0, "componentType": 5126, "count": 1,
                "type": "SCALAR",
                "sparse": {
                    "count": 1,
                    "indices": {"bufferView": 1, "byteOffset": 100,
                                "componentType": 5125},
                    "values": {"bufferView": 1, "byteOffset": 0},
                },
            }
        ],
    ), BIN8))

    # Mesh indices accessor reading through an OOB view; the index-bound scan
    # in cgltf_validate would read past the buffer allocation.
    mesh = [{"primitives": [
        {"attributes": {"POSITION": 0}, "indices": 1, "mode": 4}]}]
    add("view_oob_mesh_indices.glb", "error", glb(base_doc(
        [
            {"buffer": 0, "byteOffset": 0, "byteLength": 12},
            {"buffer": 0, "byteOffset": 0, "byteLength": 100000},
        ],
        [
            {"bufferView": 0, "componentType": 5126, "count": 1,
             "type": "VEC3"},
            {"bufferView": 1, "byteOffset": 20, "componentType": 5123,
             "count": 1, "type": "SCALAR"},
        ],
        meshes=mesh,
    ), bytes(100000)))

    # View byteOffset lands past the end of the buffer.
    add("view_byteoffset_oob.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 12, "byteLength": 4}],
        [{"bufferView": 0, "componentType": 5126, "count": 1,
          "type": "SCALAR"}],
    ), BIN8))

    # View length larger than any addressable buffer; the (clamped) huge
    # length must be rejected by the range check rather than wrap around.
    add("view_overflow.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0,
          "byteLength": 18446744073709551608}],
        [{"bufferView": 0, "componentType": 5126, "count": 1,
          "type": "SCALAR"}],
    ), BIN8))

    # OOB view with a normal accessor whose own window fits the (bogus) view
    # length; the view-level bounds check must still reject it.
    add("view_oob_accessor.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 100000}],
        [{"bufferView": 0, "componentType": 5126, "count": 1,
          "type": "SCALAR"}],
    ), BIN8))

    # Sparse values read through an OOB view.
    add("view_oob_sparse_values.glb", "error", glb(base_doc(
        [
            {"buffer": 0, "byteOffset": 0, "byteLength": 8},
            {"buffer": 0, "byteOffset": 0, "byteLength": 100000},
        ],
        [
            {
                "bufferView": 0, "componentType": 5126, "count": 1,
                "type": "SCALAR",
                "sparse": {
                    "count": 1,
                    "indices": {"bufferView": 0, "byteOffset": 0,
                                "componentType": 5125},
                    "values": {"bufferView": 1, "byteOffset": 500},
                },
            }
        ],
    ), BIN8))

    # Same OOB view, but reached through a base64 data URI buffer.
    add("view_oob_data_uri.gltf", "error", gltf_text({
        "asset": {"version": "2.0"},
        "buffers": [{
            "byteLength": 8,
            "uri": "data:application/octet-stream;base64," +
                   base64.b64encode(BIN8).decode("ascii"),
        }],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 100000}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 1,
                       "type": "SCALAR"}],
    }))

    # Same OOB view, but reached through an external .bin file buffer.
    add("view_oob_external.gltf", "error", gltf_text({
        "asset": {"version": "2.0"},
        "buffers": [{"byteLength": 8, "uri": "view_oob_external.bin"}],
        "bufferViews": [{"buffer": 0, "byteOffset": 0, "byteLength": 100000}],
        "accessors": [{"bufferView": 0, "componentType": 5126, "count": 1,
                       "type": "SCALAR"}],
    }))

    # --- Illegal byteStride --------------------------------------------

    # byteStride below the 4-byte minimum.
    add("stride_too_small.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8, "byteStride": 3}],
        [{"bufferView": 0, "componentType": 5126, "count": 1,
          "type": "VEC3"}],
    ), BIN8))

    # byteStride within [4,252] but not a multiple of 4.
    add("stride_not_multiple_of_4.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8, "byteStride": 6}],
        [{"bufferView": 0, "componentType": 5126, "count": 1,
          "type": "SCALAR"}],
    ), BIN8))

    # byteStride above the 252-byte maximum.
    add("stride_too_large.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 512, "byteStride": 256}],
        [{"bufferView": 0, "componentType": 5126, "count": 1,
          "type": "SCALAR"}],
        buffer_byte_length=512,
    ), bytes(512)))

    # --- Misaligned offsets --------------------------------------------

    # accessor byteOffset not aligned to its 4-byte component type.
    add("accessor_offset_misaligned.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8}],
        [{"bufferView": 0, "byteOffset": 2, "componentType": 5126,
          "count": 1, "type": "SCALAR"}],
    ), BIN8))

    # accessor byteOffset not aligned to its 2-byte component type.
    add("accessor_offset_misaligned_2.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8}],
        [{"bufferView": 0, "byteOffset": 1, "componentType": 5123,
          "count": 1, "type": "SCALAR"}],
    ), BIN8))

    # bufferView byteOffset is legal on its own, but view offset + accessor
    # offset is not a multiple of the 4-byte component type.
    add("view_offset_misaligned.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 1, "byteLength": 4}],
        [{"bufferView": 0, "byteOffset": 0, "componentType": 5126,
          "count": 1, "type": "SCALAR"}],
        buffer_byte_length=8,
    ), BIN8))

    # Vertex attribute byteOffset must be a multiple of 4 even when the
    # component size (1 byte here) allows arbitrary alignment.
    add("attr_offset_not_multiple_of_4.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8}],
        [{"bufferView": 0, "byteOffset": 2, "componentType": 5121,
          "count": 1, "type": "VEC4"}],
        meshes=[{"primitives": [
            {"attributes": {"POSITION": 0}, "mode": 4}]}],
    ), BIN8))

    # Sparse values byteOffset violates component alignment; the view is
    # large enough that only the alignment rule rejects it.
    add("sparse_values_misaligned.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 16}],
        [
            {
                "bufferView": 0, "componentType": 5126, "count": 1,
                "type": "SCALAR",
                "sparse": {
                    "count": 1,
                    "indices": {"bufferView": 0, "byteOffset": 0,
                                "componentType": 5125},
                    "values": {"bufferView": 0, "byteOffset": 6},
                },
            }
        ],
        buffer_byte_length=16,
    ), bytes(16)))

    # --- Overflowing sizes ----------------------------------------------

    # Accessor window stride*(count-1) overflows the size type; the
    # multiplication must be checked instead of wrapping to a small value.
    add("accessor_overflow.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8, "byteStride": 4}],
        [{"bufferView": 0, "byteOffset": 0,
          "componentType": 5126, "count": 9223372036854775807,
          "type": "SCALAR"}],
    ), BIN8))

    # Sparse count large enough that component_size * count overflows (and
    # larger than the base accessor count, which is invalid on its own).
    add("sparse_overflow.glb", "error", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8}],
        [
            {
                "bufferView": 0, "componentType": 5126, "count": 1,
                "type": "SCALAR",
                "sparse": {
                    "count": 9223372036854775807,
                    "indices": {"bufferView": 0, "byteOffset": 0,
                                "componentType": 5125},
                    "values": {"bufferView": 0, "byteOffset": 0},
                },
            }
        ],
    ), BIN8))

    # --- Valid reference documents: must keep parsing successfully -------

    # Tightly packed floats with a legal stride.
    add("valid_baseline.glb", "ok", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8, "byteStride": 4}],
        [{"bufferView": 0, "componentType": 5126, "count": 2,
          "type": "SCALAR"}],
    ), BIN8))

    # Legal maximum stride (252) and a non-zero view offset.
    add("valid_stride_max.glb", "ok", glb(base_doc(
        [{"buffer": 0, "byteOffset": 4, "byteLength": 252, "byteStride": 252}],
        [{"bufferView": 0, "componentType": 5126, "count": 1,
          "type": "SCALAR"}],
        buffer_byte_length=256,
    ), bytes(256)))

    # Legal even (2-byte) accessor offset for a 16-bit component type.
    add("valid_accessor_even_offset.glb", "ok", glb(base_doc(
        [{"buffer": 0, "byteOffset": 0, "byteLength": 8}],
        [{"bufferView": 0, "byteOffset": 2, "componentType": 5123,
          "count": 1, "type": "SCALAR"}],
    ), BIN8))

    # bufferView byteOffset that is not 4-byte aligned is legal for
    # component types whose alignment it still satisfies (2-byte here).
    add("valid_view_odd_offset.glb", "ok", glb(base_doc(
        [{"buffer": 0, "byteOffset": 2, "byteLength": 4}],
        [{"bufferView": 0, "byteOffset": 0, "componentType": 5123,
          "count": 2, "type": "SCALAR"}],
        buffer_byte_length=8,
    ), BIN8))

    return out


def extra_files():
    # External buffer referenced by view_oob_external.gltf.
    return {"view_oob_external.bin": BIN8}


def main():
    out_dir = sys.argv[1]
    os.makedirs(out_dir, exist_ok=True)
    generated = samples()
    for name, (_expect, blob) in generated.items():
        mode = "w" if name.endswith(".gltf") else "wb"
        with open(os.path.join(out_dir, name), mode) as f:
            f.write(blob)
    for name, blob in extra_files().items():
        with open(os.path.join(out_dir, name), "wb") as f:
            f.write(blob)
    with open(os.path.join(out_dir, "manifest.txt"), "w", newline="\n") as f:
        f.write("# <file> <ok|error> — expected outcome of "
                "parse + load_buffers + validate\n")
        for name in sorted(generated):
            f.write("%s %s\n" % (name, generated[name][0]))
    print("wrote %d samples to %s" % (len(generated), out_dir))


if __name__ == "__main__":
    main()
