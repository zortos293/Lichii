"""Layout of the 112 hashed bytes of FixedPipelineState (build-88, dynamic vertex input on)."""
import struct

STATE_OFF = 48          # unique_hashes[6] come first in the 1056-byte graphics key
STATE_LEN = 112


def u32(b, o):
    return struct.unpack_from('<I', b, o)[0]


def split(state: bytes):
    """Returns (vertex part, fragment/target part, scene part) as hashable tuples."""
    raw1, raw2 = u32(state, 0), u32(state, 4)
    v = (raw1 & 0x0F0FFFC0,            # topology, xfb, ndc, polygon mode, tessellation, patch points
         u32(state, 28),               # point size
         state[64:72])                 # attribute types
    f = (raw1 & 0xF0200000,            # msaa mode, dual source blend
         raw2 & 0x000187FE,            # alpha test, early z, depth enable/format, alpha to coverage/one
         state[8:16],                  # colour formats
         u32(state, 24),               # alpha test reference
         state[80:112])                # blending attachments
    s = (raw1 & 0x0000003F, raw1 & 0x00100000,  # dynamic-state feature bits, cwe dynamic
         raw2 & 0xFFFE7801,            # y negate, provoking vertex, conservative, smooth lines, app stage
         state[16:24],                 # driver
         state[32:64],                 # viewport swizzles
         state[72:80])                 # dynamic_state raw1/raw2
    return v, f, s


def join(v, f, s) -> bytes:
    raw1 = v[0] | f[0] | s[0] | s[1]
    raw2 = f[1] | s[2]
    b = bytearray(STATE_LEN)
    struct.pack_into('<II', b, 0, raw1, raw2)
    b[8:16] = f[2]
    b[16:24] = s[3]
    struct.pack_into('<II', b, 24, f[3], v[1])
    b[32:64] = s[4]
    b[64:72] = v[2]
    b[72:80] = s[5]
    b[80:112] = f[4]
    return bytes(b)


def attribute_read_mask(sph: bytes) -> int:
    """Generic attributes a vertex program reads, from its header's input map (4 bits each)."""
    m = 0
    for i in range(16):
        b = sph[0x18 + i]
        for half in range(2):
            if (b >> (4 * half)) & 0xF:
                m |= 1 << (i * 2 + half)
    return m


EDS3_BLEND = 1 << 3            # raw1: extended_dynamic_state_3_blend
DYNAMIC_VERTEX_INPUT = 1 << 5  # raw1: dynamic_vertex_input
XFB_ENABLED = 1 << 6           # raw1: xfb_enabled


def hashes_past_prefix(state: bytes) -> bool:
    """Whether the key's hash covers bytes past these 112 (FixedPipelineState::Size): with
    transform feedback, or without dynamic vertex input (the attributes and divisors)."""
    raw1 = u32(state, 0)
    return bool(raw1 & XFB_ENABLED) or not raw1 & DYNAMIC_VERTEX_INPUT


def canonical(state: bytes, read_mask: int | None) -> bytes:
    """State with the bytes that cannot change the pipeline cleared, as
    FixedPipelineState::CanonicalizeNative does: attribute types the vertex program does not read
    (only with dynamic vertex input; without it these bytes are the enabled divisors) and, unless
    blending is dynamic, blend equations/factors of attachments whose blending is off and
    attachments past the last colour target."""
    b = bytearray(state)
    raw1 = u32(b, 0)
    if read_mask is not None and raw1 & DYNAMIC_VERTEX_INPUT:
        types = int.from_bytes(b[64:72], 'little')
        keep = 0
        for a in range(32):
            if read_mask >> a & 1:
                keep |= 3 << (2 * a)
        b[64:72] = (types & keep).to_bytes(8, 'little')
    if raw1 & EDS3_BLEND:
        return bytes(b)
    used = 0
    for i in range(8):
        if b[8 + i]:
            used = i + 1
    for i in range(8):
        att = u32(b, 80 + 4 * i)
        if i >= used:
            att = 0           # past the last colour target: not in the pipeline
        elif not (att >> 30) & 1:
            att &= 0xF        # blending off: keep the colour write mask only
        struct.pack_into('<I', b, 80 + 4 * i, att)
    return bytes(b)
