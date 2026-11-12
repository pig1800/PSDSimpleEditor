// D3D12 compute kernels: PSD channel data -> premultiplied RGBA8 -> fitted RGBA8.
// Adapted from PsdPreview's gpu_d3d12.hlsl (same unpack and resize algorithms), with:
//   - pp_unpack: a raw-rows mode (uncompressed / inflated ZIP channels) and a row base, so each
//     channel is its own dispatch (channels may use different compressions);
//   - pp_merge: 8-bit planes (gray or RGB, optional transparency) -> premultiplied RGBA8, the one
//     pixel format the renderer accepts;
//   - pp_resize: RGBA8 in, RGBA8 out (alpha kept), nothing else.
// The build compiles each entry point with dxc into a DXIL header:
//   dxc -T cs_6_0 -E <entry> -D PP_<KERNEL> [-D FIXED=n] -Fh <header> -Vn <array>
// All buffers are raw (ByteAddressBuffer), bound as root descriptors: no descriptor heaps.

#define PP_RS "RootConstants(num32BitConstants=12, b0), SRV(t0), SRV(t1), UAV(u0), UAV(u1)"

// ---- status ----------------------------------------------------------------------------------
// u1 holds the first corrupt row (0xFFFFFFFF = none); pp_clear resets it before each decode.

#if defined(PP_CLEAR)
RWByteAddressBuffer status : register(u1);

[RootSignature(PP_RS)]
[numthreads(1, 1, 1)]
void pp_clear() { status.Store(0, 0xFFFFFFFFu); }
#endif

// ---- PackBits / raw rows -----------------------------------------------------------------------
//
// Row r = rowBase + i (i < rows): source bytes comp[offsets[r] .. offsets[r + 1]), rowBytes bytes
// unpacked, written as `width` bytes to planes + r * pitch (pitch: a multiple of 16; the padding
// is zeroed). 16-bit samples (shift 1) keep their first, i.e. high, byte. raw != 0: the row is
// stored uncompressed. A run past the row end is cut; a short or malformed row is zero-filled and
// the lowest such row is left in status.
//
// One thread per row, decoding it front to back; each thread reads and writes 16 bytes at a time.

#if defined(PP_UNPACK)
ByteAddressBuffer comp : register(t0);
ByteAddressBuffer offsets : register(t1);
RWByteAddressBuffer planes : register(u0);
RWByteAddressBuffer status : register(u1);
cbuffer Unpack : register(b0) {
    uint rows, rowBytes, shift, width, pitch;
    uint groupsPerX;  // a dispatch has at most 65535 groups per axis
    uint rowBase, raw;
};

#define GROUP 64

// Source bytes, through a 16-byte block cache. (The buffer has 16 bytes of slack.)
struct Reader {
    uint4 block;
    uint at;  // address of `block`
    uint get(uint a) {
        if ((a & ~15u) != at) {
            at = a & ~15u;
            block = comp.Load4(at);
        }
        const uint i = (a >> 2) & 3u;
        const uint w = i == 0 ? block.x : i == 1 ? block.y : i == 2 ? block.z : block.w;
        return (w >> ((a & 3u) * 8u)) & 0xFFu;
    }
};

// Output bytes, assembled into words and stored 16 bytes at a time.
struct Writer {
    uint4 block;
    uint words, word, n;  // complete words in `block`; bytes in `word`
    uint addr;            // where `block` goes
    void put_word(uint w) {
        if (words == 0) block.x = w;
        else if (words == 1) block.y = w;
        else if (words == 2) block.z = w;
        else block.w = w;
        if (++words == 4) {
            planes.Store4(addr, block);
            addr += 16;
            words = 0;
        }
    }
    void put(uint b) {
        word |= b << (8u * n);
        if (++n == 4) {
            put_word(word);
            word = 0;
            n = 0;
        }
    }
    // The last, partial word and block; then zeros up to `end`.
    void finish(uint end) {
        if (n != 0) put_word(word);
        if (words != 0) {
            if (words < 2) block.y = 0;
            if (words < 3) block.z = 0;
            block.w = 0;
            planes.Store4(addr, block);
            addr += 16;
        }
        for (; addr < end; addr += 16) planes.Store4(addr, uint4(0, 0, 0, 0));
    }
};

[RootSignature(PP_RS)]
[numthreads(GROUP, 1, 1)]
void pp_unpack(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    const uint i = (group.y * groupsPerX + group.x) * GROUP + lane;
    if (i >= rows) return;
    const uint r = rowBase + i;
    uint src = offsets.Load(r * 4);
    const uint end = offsets.Load(r * 4 + 4);
    Reader rd;
    rd.at = 0xFFFFFFFFu;
    rd.block = uint4(0, 0, 0, 0);
    Writer wr;
    wr.block = uint4(0, 0, 0, 0);
    wr.words = 0;
    wr.word = 0;
    wr.n = 0;
    wr.addr = r * pitch;
    const uint keep = (1u << shift) - 1u;  // unpacked byte k is kept if (k & keep) == 0
    bool bad = false;
    if (raw != 0) {
        if (end - src < rowBytes) {
            bad = true;
        } else {
            [loop] for (uint k = 0; k < rowBytes; k += keep + 1) wr.put(rd.get(src + k));
        }
    } else {
        uint k = 0;  // unpacked bytes done
        [loop] while (k < rowBytes) {
            if (src >= end) { bad = true; break; }
            const uint b = rd.get(src++);
            if (b == 0x80) continue;  // no-op
            if (b < 0x80) {           // literal: the next b + 1 bytes
                const uint len = b + 1;
                if (len > end - src) { bad = true; break; }
                const uint m = min(len, rowBytes - k);
                [loop] for (uint j = 0; j < m; ++j)
                    if (((k + j) & keep) == 0) wr.put(rd.get(src + j));
                src += len;
                k += m;
            } else {  // repeat: the next byte 257 - b times
                if (src >= end) { bad = true; break; }
                const uint v = rd.get(src++);
                const uint m = min(257 - b, rowBytes - k);
                uint count = ((k + m + keep) >> shift) - ((k + keep) >> shift);  // kept bytes among them
                k += m;
                [loop] for (; count > 0 && wr.n != 0; --count) wr.put(v);
                const uint vvvv = v * 0x01010101u;
                [loop] for (; count >= 4; count -= 4) wr.put_word(vvvv);
                [loop] for (; count > 0; --count) wr.put(v);
            }
        }
    }
    wr.finish(r * pitch + pitch);  // short/malformed row: zeros; and the padding
    if (bad) {
        uint previous;
        status.InterlockedMin(0, r, previous);
    }
}
#endif

// ---- Merge -------------------------------------------------------------------------------------
//
// Planes (w x h bytes, rows `pitch` apart, planes `planeStride` apart): colour plane(s) 0 (gray)
// or 0, 1, 2 (RGB), transparency at plane `alphaPlane` (0xFFFFFFFF: opaque) -> premultiplied
// RGBA8, rows `dstPitch` apart. One thread per 4 pixels (one 32-bit load per plane, one 16-byte
// store); the last quad of a row reads plane padding and writes inside the row pitch.

#if defined(PP_MERGE)
ByteAddressBuffer planes : register(t0);
RWByteAddressBuffer rgba : register(u0);
cbuffer Merge : register(b0) {
    uint w, h, pitch, planeStride;
    uint gray, alphaPlane, dstPitch, quads;  // quads = ceil(w / 4)
    uint groupsPerX;
};

uint premul(uint c, uint a) { return (c * a + 127u) / 255u; }

[RootSignature(PP_RS)]
[numthreads(64, 1, 1)]
void pp_merge(uint3 group : SV_GroupID, uint lane : SV_GroupIndex) {
    const uint i = (group.y * groupsPerX + group.x) * 64 + lane;
    const uint y = i / quads;
    if (y >= h) return;
    const uint x = (i - y * quads) * 4;
    const uint a0 = y * pitch + x;
    const uint r = planes.Load(a0);
    const uint g = gray != 0 ? r : planes.Load(a0 + planeStride);
    const uint b = gray != 0 ? r : planes.Load(a0 + 2 * planeStride);
    const uint a = alphaPlane != 0xFFFFFFFFu ? planes.Load(a0 + alphaPlane * planeStride) : 0xFFFFFFFFu;
    uint o[4];
    [unroll] for (uint k = 0; k < 4; ++k) {
        const uint s = 8u * k;
        const uint ak = (a >> s) & 0xFFu;
        o[k] = premul((r >> s) & 0xFFu, ak) | premul((g >> s) & 0xFFu, ak) << 8 |
               premul((b >> s) & 0xFFu, ak) << 16 | ak << 24;
    }
    rgba.Store4(y * dstPitch + x * 4, uint4(o[0], o[1], o[2], o[3]));
}
#endif

// ---- Resize -----------------------------------------------------------------------------------
//
// PsdPreview's filter and single-kernel separable scheme: Mitchell-Netravali cubic (B, C),
// support widened by the reduction factor when shrinking, pixel centres at (i + 0.5) * scale -
// 0.5, border pixels replicated, weights normalised. A 32 x 8 group makes 32 x 8 output pixels,
// streaming the source rows they need through groupshared memory, filtered horizontally once per
// group. RGBA8 (w x h, rows `pitch` apart) -> RGBA8 (dw x dh, rows `dstPitch` apart).

#if defined(PP_RESIZE)
ByteAddressBuffer src : register(t0);
RWByteAddressBuffer dst : register(u0);
cbuffer Resize : register(b0) {
    uint w, h, pitch, dw, dh, dstPitch;
    float B, C;
};

#define TILE_W 32
#define TILE_H 8
#define CHUNK 40  // source rows per pass through groupshared memory
#define TAPS 64   // horizontal taps per column the weight table holds (shrinking up to ~15x)

groupshared float4 H[CHUNK][TILE_W];      // horizontally filtered source rows (20 KB)
groupshared float WX[TILE_W][TAPS + 1];   // each column's horizontal weights (+ 1: no bank conflicts)

// 6 x the Mitchell-Netravali kernel (the normalisation divides the 6 out).
float cubic6(float x) {
    x = abs(x);
    if (x < 1.0f) return ((12.0f - 9.0f * B - 6.0f * C) * x + (-18.0f + 12.0f * B + 6.0f * C)) * x * x + (6.0f - 2.0f * B);
    if (x < 2.0f) return (((-B - 6.0f * C) * x + (6.0f * B + 30.0f * C)) * x + (-12.0f * B - 48.0f * C)) * x + (8.0f * B + 24.0f * C);
    return 0.0f;
}

uint to_u8(float v) { return uint(clamp(round(v), 0.0f, 255.0f)); }

[RootSignature(PP_RS)]
[numthreads(TILE_W, TILE_H, 1)]
void pp_resize(uint3 group : SV_GroupID, uint3 t : SV_GroupThreadID) {
    const int tx = int(t.x), ty = int(t.y);
    const int iw = int(w), ih = int(h), idw = int(dw), idh = int(dh);
    const int x = int(group.x) * TILE_W + tx, y = int(group.y) * TILE_H + ty;
    const int yLast = min(int(group.y) * TILE_H + TILE_H, idh) - 1;  // the group's last output row

    const float sx = float(w) / float(dw), sy = float(h) / float(dh);
    const float fx = max(sx, 1.0f), fy = max(sy, 1.0f);  // kernel half-width / 2 in source pixels
    const float ix = 1.0f / fx, iy = 1.0f / fy;
    const float cx = (float(x) + 0.5f) * sx - 0.5f, cy = (float(y) + 0.5f) * sy - 0.5f;
    const int x0 = int(ceil(cx - 2.0f * fx)), x1 = int(floor(cx + 2.0f * fx));
    const int y0 = int(ceil(cy - 2.0f * fy)), y1 = int(floor(cy + 2.0f * fy));
    // Source rows the whole group needs (one spare row each side against rounding differences).
    const int r0 = int(ceil((float(group.y * TILE_H) + 0.5f) * sy - 0.5f - 2.0f * fy)) - 1;
    const int r1 = int(floor((float(yLast) + 0.5f) * sy - 0.5f + 2.0f * fy)) + 1;

    float wsx = 0.0f;
    for (int i = x0; i <= x1; ++i) wsx += cubic6((float(i) - cx) * ix);
    // The horizontal weights depend only on the column: computed once into the table. With FIXED
    // (the host picks the variant whose tap count covers the scale) the tap loop has that many
    // iterations, unrolled, the table zero-padded (a run-time count was 2.2x slower on an iGPU).
    const bool table = int(4.0f * fx) + 2 <= TAPS;
#ifdef FIXED
    for (int k = ty; k < FIXED; k += TILE_H) WX[tx][k] = k <= x1 - x0 ? cubic6((float(x0 + k) - cx) * ix) : 0.0f;
#else
    if (table)
        for (int k = ty; k <= x1 - x0; k += TILE_H) WX[tx][k] = cubic6((float(x0 + k) - cx) * ix);
#endif
    GroupMemoryBarrierWithGroupSync();

    float4 acc = 0.0f;
    float wsy = 0.0f;
    for (int c0 = r0; c0 <= r1; c0 += CHUNK) {  // every thread runs the same chunks (barriers)
        const int n = min(CHUNK, r1 - c0 + 1);
        for (int r = ty; r < n && x < idw; r += TILE_H) {  // columns past dw have nothing to filter
            const uint rowAddr = uint(clamp(c0 + r, 0, ih - 1)) * pitch;
            float4 s = 0.0f;
#ifdef FIXED
            [unroll] for (int k = 0; k < FIXED; ++k) {
                const int i = x0 + k;
                const float wx = WX[tx][k];
#else
            for (int i = x0; i <= x1; ++i) {
                const float wx = table ? WX[tx][i - x0] : cubic6((float(i) - cx) * ix);
#endif
                const uint v = src.Load(rowAddr + 4u * uint(clamp(i, 0, iw - 1)));
                s += wx * float4(float(v & 0xFFu), float((v >> 8) & 0xFFu), float((v >> 16) & 0xFFu), float(v >> 24));
            }
            H[r][tx] = s;
        }
        GroupMemoryBarrierWithGroupSync();
        const int jEnd = min(y1, c0 + n - 1);
        for (int j = max(y0, c0); j <= jEnd; ++j) {
            const float wy = cubic6((float(j) - cy) * iy);
            if (wy == 0.0f) continue;
            wsy += wy;
            acc += wy * H[j - c0][tx];
        }
        GroupMemoryBarrierWithGroupSync();
    }
    if (x >= idw || y >= idh) return;
    const float4 v = acc * (1.0f / (wsx * wsy));
    const uint a = to_u8(v.w);
    // Premultiplied: a sharpening filter's overshoot must not push a colour above its alpha.
    const uint p = min(to_u8(v.x), a) | min(to_u8(v.y), a) << 8 | min(to_u8(v.z), a) << 16 | a << 24;
    dst.Store(uint(y) * dstPitch + uint(x) * 4, p);
}
#endif
