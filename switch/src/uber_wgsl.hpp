#pragma once

#include "uber_layout.hpp"

#include <dolphin/gx/GXEnum.h>

#include <string>
#include <string_view>

// The uber-shader: one WGSL program that interprets a draw's whole GX state (TEV,
// texgen, lighting, vertex formats, fog, alpha compare) from the uniform block in
// uber_layout.hpp. It exists so a draw whose specialised shader is still compiling
// can be drawn right away instead of waiting or being skipped. It mirrors the
// semantics of the generated shaders in shader.cpp; anything it does not cover is
// refused by uber_supports() and takes the specialised path.
namespace melee_nx::uber {

inline std::string replace_all(std::string text, std::string_view from, const std::string& to) {
    for (size_t at = text.find(from); at != std::string::npos; at = text.find(from, at + to.size()))
        text.replace(at, from.size(), to);
    return text;
}

// `helpers` is the shared fetch/overflow WGSL (bswap, fetch_*, tev_overflow_*)
// that the generated shaders carry too.
inline std::string uber_shader_source(const std::string& helpers) {
    // Constants are emitted as WGSL `const`s so the body reads like the generator.
    std::string consts;
    const auto add = [&consts](const char* name, uint32_t value) {
        consts += "const ";
        consts += name;
        consts += ": u32 = ";
        consts += std::to_string(value);
        consts += "u;\n";
    };
    add("F_VIEWPORT", F_VIEWPORT);
    add("F_PROJ", F_PROJ);
    add("F_POSTEX", F_POSTEX);
    add("F_NRM", F_NRM);
    add("F_PT", F_PT);
    add("F_LIGHTS", F_LIGHTS);
    add("F_CC", F_CC);
    add("F_TEVREG", F_TEVREG);
    add("F_KCOLOR", F_KCOLOR);
    add("F_FOG", F_FOG);
    add("F_TCSCALE", F_TCSCALE);
    add("F_TEXSB", F_TEXSB);
    add("U_HDR0", U_HDR0);
    add("U_HDR1", U_HDR1);
    add("U_HDR2", U_HDR2);
    add("U_LIGHTMASK", U_LIGHTMASK);
    add("U_ATTRS", U_ATTRS);
    add("U_SWAP", U_SWAP);
    add("U_CHAN", U_CHAN);
    add("U_TCG", U_TCG);
    add("U_TEV", U_TEV);
    add("GX_VA_POS", GX_VA_POS);
    add("GX_VA_NRM", GX_VA_NRM);
    add("GX_VA_CLR0", GX_VA_CLR0);
    add("GX_VA_TEX0", GX_VA_TEX0);
    add("GX_VA_TEX0MTXIDX", GX_VA_TEX0MTXIDX);
    add("GX_INDEX8", GX_INDEX8);
    add("GX_INDEX16", GX_INDEX16);
    add("TG_MTX3x4", GX_TG_MTX3x4);
    add("TG_MTX2x4", GX_TG_MTX2x4);
    add("TG_SRTG", GX_TG_SRTG);
    add("TG_POS", GX_TG_POS);
    add("TG_NRM", GX_TG_NRM);
    add("TG_TEX0", GX_TG_TEX0);
    add("TG_COLOR0", GX_TG_COLOR0);
    add("TG_COLOR1", GX_TG_COLOR1);
    add("TG_IDENTITY", GX_IDENTITY);
    add("TG_PTIDENTITY", GX_PTIDENTITY);
    add("TG_PTTEXMTX0", GX_PTTEXMTX0);
    add("CH_ZERO", GX_COLOR_ZERO);

    static constexpr std::string_view body = R"WGSL(
struct Immediate {
    vtx_start: u32,
    current_pnmtx: u32,
    fog_range_base: u32,
    _pad: u32,
    array_start0: vec4u,
    array_start1: vec4u,
    array_start2: vec4u,
};
var<immediate> imm: Immediate;

struct UberUniform {
    f: array<vec4f, @F_COUNT@>,
    u: array<vec4u, @U_COUNT@>,
};
@group(0) @binding(0)
var<storage, read> vbuf: array<u32>;
@group(0) @binding(1)
var<storage, read> abuf: array<u32>;
@group(1) @binding(0)
var<uniform> ubuf: UberUniform;

@group(2) @binding(0) var tex0: texture_2d<f32>;
@group(2) @binding(1) var tex0_samp: sampler;
@group(2) @binding(2) var tex1: texture_2d<f32>;
@group(2) @binding(3) var tex1_samp: sampler;
@group(2) @binding(4) var tex2: texture_2d<f32>;
@group(2) @binding(5) var tex2_samp: sampler;
@group(2) @binding(6) var tex3: texture_2d<f32>;
@group(2) @binding(7) var tex3_samp: sampler;
@group(2) @binding(8) var tex4: texture_2d<f32>;
@group(2) @binding(9) var tex4_samp: sampler;
@group(2) @binding(10) var tex5: texture_2d<f32>;
@group(2) @binding(11) var tex5_samp: sampler;
@group(2) @binding(12) var tex6: texture_2d<f32>;
@group(2) @binding(13) var tex6_samp: sampler;
@group(2) @binding(14) var tex7: texture_2d<f32>;
@group(2) @binding(15) var tex7_samp: sampler;

fn uf(i: u32) -> vec4f { return ubuf.f[i]; }
fn mat3x4_at(i: u32) -> mat3x4f { return mat3x4f(ubuf.f[i], ubuf.f[i + 1u], ubuf.f[i + 2u]); }
// A uniform vector's component by a runtime index, through constant swizzles: uam (deko3d) reads .x for
// `v[i]` on a uniform or immediate vector.
fn pick4u(v: vec4u, i: u32) -> u32 {
    return select(select(select(v.x, v.y, i == 1u), v.z, i == 2u), v.w, i == 3u);
}
fn uw(vi: u32, c: u32) -> u32 { return pick4u(ubuf.u[vi], c); }

// ---- Vertex attribute access ----------------------------------------------

struct Attr {
    atype: u32,
    cnt: u32,
    ctype: u32,
    offs: u32,
    stride: u32,
    frac: u32,
    le: bool,
};

fn attr_desc(a: u32) -> Attr {
    let k0 = a * 2u;
    let k1 = k0 + 1u;
    let w0 = pick4u(ubuf.u[U_ATTRS + (k0 >> 2u)], k0 & 3u);
    let w1 = pick4u(ubuf.u[U_ATTRS + (k1 >> 2u)], k1 & 3u);
    var d: Attr;
    d.atype = w0 & 0xFFu;
    d.cnt = (w0 >> 8u) & 0xFFu;
    d.ctype = (w0 >> 16u) & 0xFFu;
    d.offs = (w0 >> 24u) & 0xFFu;
    d.stride = w1 & 0xFFu;
    d.frac = (w1 >> 8u) & 0xFFu;
    d.le = ((w1 >> 16u) & 1u) != 0u;
    return d;
}

fn array_start(a: u32) -> u32 {
    let g = a - GX_VA_POS;
    if (g < 4u) { return pick4u(imm.array_start0, g); }
    if (g < 8u) { return pick4u(imm.array_start1, g - 4u); }
    return pick4u(imm.array_start2, g - 8u);
}

struct AttrLoc {
    offs: u32,
    from_array: bool,
    le: bool,
};

fn attr_loc(d: Attr, a: u32, vidx: u32) -> AttrLoc {
    var loc: AttrLoc;
    let base = imm.vtx_start + vidx * ubuf.u[U_HDR0].x + d.offs;
    if (d.atype == GX_INDEX8) {
        let idx = raw_fetch_u8_1(&vbuf, base);
        loc.offs = array_start(a) + idx * d.stride;
        loc.from_array = true;
        loc.le = d.le;
    } else if (d.atype == GX_INDEX16) {
        let idx = raw_fetch_u16_1(&vbuf, base, false);
        loc.offs = array_start(a) + idx * d.stride;
        loc.from_array = true;
        loc.le = d.le;
    } else {
        loc.offs = base;
        loc.from_array = false;
        loc.le = false;
    }
    return loc;
}

fn fetch_comp(p: ptr<storage, array<u32>>, d: Attr, o: u32, le: bool) -> vec4f {
    let frac = d.frac;
    switch d.ctype {
        case 0u: {
            if (d.cnt == 1u) { return vec4f(fetch_u8_1(p, o, frac, le), 0.0, 0.0, 0.0); }
            if (d.cnt == 2u) { return vec4f(fetch_u8_2(p, o, frac, le), 0.0, 0.0); }
            if (d.cnt == 3u) { return vec4f(fetch_u8_3(p, o, frac, le), 0.0); }
            return fetch_u8_4(p, o, frac, le);
        }
        case 1u: {
            if (d.cnt == 1u) { return vec4f(fetch_s8_1(p, o, frac, le), 0.0, 0.0, 0.0); }
            if (d.cnt == 2u) { return vec4f(fetch_s8_2(p, o, frac, le), 0.0, 0.0); }
            if (d.cnt == 3u) { return vec4f(fetch_s8_3(p, o, frac, le), 0.0); }
            return fetch_s8_4(p, o, frac, le);
        }
        case 2u: {
            if (d.cnt == 1u) { return vec4f(fetch_u16_1(p, o, frac, le), 0.0, 0.0, 0.0); }
            if (d.cnt == 2u) {
                return vec4f(fetch_u16_1(p, o, frac, le), fetch_u16_1(p, o + 2u, frac, le), 0.0, 0.0);
            }
            if (d.cnt == 3u) {
                return vec4f(fetch_u16_1(p, o, frac, le), fetch_u16_1(p, o + 2u, frac, le),
                             fetch_u16_1(p, o + 4u, frac, le), 0.0);
            }
            return fetch_u16_4(p, o, frac, le);
        }
        case 3u: {
            if (d.cnt == 1u) { return vec4f(fetch_s16_1(p, o, frac, le), 0.0, 0.0, 0.0); }
            if (d.cnt == 2u) {
                return vec4f(fetch_s16_1(p, o, frac, le), fetch_s16_1(p, o + 2u, frac, le), 0.0, 0.0);
            }
            if (d.cnt == 3u) {
                return vec4f(fetch_s16_1(p, o, frac, le), fetch_s16_1(p, o + 2u, frac, le),
                             fetch_s16_1(p, o + 4u, frac, le), 0.0);
            }
            return fetch_s16_4(p, o, frac, le);
        }
        case 4u: {
            if (d.cnt == 1u) { return vec4f(fetch_f32_1(p, o, le), 0.0, 0.0, 0.0); }
            if (d.cnt == 2u) { return vec4f(fetch_f32_2(p, o, le), 0.0, 0.0); }
            if (d.cnt == 3u) { return vec4f(fetch_f32_3(p, o, le), 0.0); }
            return fetch_f32_4(p, o, le);
        }
        default: { return vec4f(0.0); }
    }
}

fn fetch_color_comp(p: ptr<storage, array<u32>>, ct: u32, o: u32, le: bool) -> vec4f {
    switch ct {
        case 0u: { return fetch_rgb565(p, o, le); }
        case 1u: { return fetch_rgb8(p, o, le); }
        case 2u: { return fetch_rgbx8(p, o, le); }
        case 3u: { return fetch_rgba4(p, o, le); }
        case 4u: { return fetch_rgba6(p, o, le); }
        case 5u: { return fetch_rgba8(p, o, le); }
        default: { return vec4f(0.0); }
    }
}

// Numeric attribute (position, normal, texcoord): unused components are zero.
fn load_attr(a: u32, vidx: u32) -> vec4f {
    let d = attr_desc(a);
    if (d.atype == 0u) { return vec4f(0.0); }
    let loc = attr_loc(d, a, vidx);
    if (loc.from_array) { return fetch_comp(&abuf, d, loc.offs, loc.le); }
    return fetch_comp(&vbuf, d, loc.offs, false);
}

fn load_color(a: u32, vidx: u32) -> vec4f {
    let d = attr_desc(a);
    if (d.atype == 0u) { return vec4f(0.0); }
    let loc = attr_loc(d, a, vidx);
    if (loc.from_array) { return fetch_color_comp(&abuf, d.ctype, loc.offs, loc.le); }
    return fetch_color_comp(&vbuf, d.ctype, loc.offs, false);
}

fn load_byte_attr(a: u32, vidx: u32) -> u32 {
    let d = attr_desc(a);
    let loc = attr_loc(d, a, vidx);
    if (loc.from_array) { return raw_fetch_u8_1(&abuf, loc.offs); }
    return raw_fetch_u8_1(&vbuf, loc.offs);
}

// ---- Lighting ---------------------------------------------------------------

// One channel's lit color for the rgb (alpha=false) or alpha pass; the caller
// keeps rgb or .a. `clr` is the vertex color feeding the channel.
fn light_channel(c: u32, alpha: bool, mv_pos: vec3f, mv_nrm: vec3f, clr: vec4f) -> vec4f {
    let cfg_index = select(c, c + 2u, alpha);
    let cfg = pick4u(ubuf.u[U_CHAN], cfg_index);
    let mat_vtx = (cfg & 1u) != 0u;
    let amb_vtx = ((cfg >> 1u) & 1u) != 0u;
    let diff_fn = (cfg >> 2u) & 3u;
    let attn_fn = (cfg >> 4u) & 3u;
    let lit = ((cfg >> 6u) & 1u) != 0u;
    let matc = select(uf(F_CC + c * 4u + select(1u, 3u, alpha)), clr, mat_vtx);
    if (!lit) { return matc; }
    var lighting = select(uf(F_CC + c * 4u + select(0u, 2u, alpha)), clr, amb_vtx);
    let mask = pick4u(ubuf.u[U_LIGHTMASK], cfg_index);
    for (var i = 0u; i < 8u; i++) {
        if ((mask & (1u << i)) == 0u) { continue; }
        let lb = F_LIGHTS + i * 5u;
        let lpos = uf(lb).xyz;
        let ldir_w = uf(lb + 1u).xyz;
        let lcolor = uf(lb + 2u);
        let cos_att = uf(lb + 3u).xyz;
        let dist_att = uf(lb + 4u).xyz;
        var ldir = lpos - mv_pos;
        let dist2 = dot(ldir, ldir);
        let dist = sqrt(dist2);
        ldir = ldir / dist;
        var attn: f32;
        if (attn_fn == 2u) {
            attn = 1.0;
        } else if (attn_fn == 1u) {
            let cosine = max(0.0, dot(ldir, ldir_w));
            let cos_attn = dot(cos_att, vec3f(1.0, cosine, cosine * cosine));
            let dist_attn = dot(dist_att, vec3f(1.0, dist, dist2));
            attn = max(0.0, cos_attn / dist_attn);
        } else {
            attn = select(0.0, max(0.0, dot(mv_nrm, ldir_w)), dot(mv_nrm, ldir) >= 0.0);
            let cos_attn = dot(cos_att, vec3f(1.0, attn, attn * attn));
            var dist_attn: f32;
            if (diff_fn != 0u) {
                dist_attn = max(0.0, dot(normalize(dist_att), vec3f(1.0, attn, attn * attn)));
            } else {
                dist_attn = max(0.0, dot(dist_att, vec3f(1.0, attn, attn * attn)));
            }
            attn = max(0.0, cos_attn / dist_attn);
        }
        var diff = 1.0;
        if (diff_fn == 1u) {
            diff = dot(ldir, mv_nrm);
        } else if (diff_fn == 2u) {
            diff = max(0.0, dot(ldir, mv_nrm));
        }
        lighting = lighting + (attn * diff * lcolor);
    }
    return matc * clamp(lighting, vec4f(0.0), vec4f(1.0));
}

// ---- Vertex stage ---------------------------------------------------------

struct VertexOutput {
    @builtin(position) pos: vec4f,
    @location(0) cc0: vec4f,
    @location(1) cc1: vec4f,
    @location(2) tex0: vec3f,
    @location(3) tex1: vec3f,
    @location(4) tex2: vec3f,
    @location(5) tex3: vec3f,
    @location(6) tex4: vec3f,
    @location(7) tex5: vec3f,
    @location(8) tex6: vec3f,
    @location(9) tex7: vec3f,
};

// One texture coordinate's interpolant. Called with a constant index so nothing is
// held in a dynamically indexed local array (which the GPU would spill to memory).
fn texgen_one(i: u32, tex_mask: u32, pos: vec3f, nrm: vec3f, clr0: vec4f, clr1: vec4f, vidx: u32) -> vec3f {
    if ((tex_mask & (1u << i)) == 0u) { return vec3f(0.0); }
    let g = ubuf.u[U_TCG + i];
    let ttype = g.x;
    let tsrc = g.y;
    let tmtx = g.z;
    let tpost = g.w & 0xFFu;
    let tnorm = ((g.w >> 8u) & 1u) != 0u;
    var tc = vec4f(0.0);
    if (tsrc >= TG_TEX0 && tsrc < TG_TEX0 + 8u) {
        tc = vec4f(load_attr(GX_VA_TEX0 + (tsrc - TG_TEX0), vidx).xy, 1.0, 1.0);
    } else if (tsrc == TG_POS) {
        tc = vec4f(pos, 1.0);
    } else if (tsrc == TG_NRM) {
        tc = vec4f(nrm, 1.0);
    } else if (tsrc == TG_COLOR0) {
        tc = clr0;
    } else if (tsrc == TG_COLOR1) {
        tc = clr1;
    }
    var tmp = vec3f(0.0);
    if (ttype == TG_MTX2x4 || ttype == TG_MTX3x4) {
        if (attr_desc(GX_VA_TEX0MTXIDX + i).atype != 0u) {
            tmp = tc * mat3x4_at(F_POSTEX + (load_byte_attr(GX_VA_TEX0MTXIDX + i, vidx) / 3u) * 3u);
        } else if (tmtx == TG_IDENTITY) {
            tmp = tc.xyz;
        } else {
            tmp = tc * mat3x4_at(F_POSTEX + (tmtx / 3u) * 3u);
        }
        if (ttype == TG_MTX2x4) { tmp.z = 1.0; }
    } else if (ttype == TG_SRTG) {
        tmp = vec3f(tc.xy, 1.0);
    }
    if (tnorm) {
        tmp = select(tmp, normalize(tmp), dot(tmp, tmp) > 1e-10);
    }
    var proj = tmp;
    if (tpost != TG_PTIDENTITY) {
        proj = vec4f(tmp, 1.0) * mat3x4_at(F_PT + ((tpost - TG_PTTEXMTX0) / 3u) * 3u);
    }
    if (ttype == TG_MTX3x4) {
        if (proj.z == 0.0) {
            proj = vec3f(clamp(proj.xy * 0.5, vec2f(-1.0), vec2f(1.0)), 0.0);
        }
        return proj;
    }
    return vec3f(proj.xy, 0.0);
}

@vertex
fn vs_main(@builtin(vertex_index) vidx: u32) -> VertexOutput {
    var out: VertexOutput;
    var pnmtx = imm.current_pnmtx;
    if (attr_desc(0u).atype != 0u) {
        pnmtx = load_byte_attr(0u, vidx) / 3u;
    }
    let pos = load_attr(GX_VA_POS, vidx).xyz;
    let mv_pos = vec4f(pos, 1.0) * mat3x4_at(F_POSTEX + pnmtx * 3u);
    out.pos = vec4f(mv_pos, 1.0) * mat4x4f(uf(F_PROJ), uf(F_PROJ + 1u), uf(F_PROJ + 2u), uf(F_PROJ + 3u));

    var nrm = vec3f(1.0, 0.0, 0.0);
    if (attr_desc(GX_VA_NRM).atype != 0u) {
        nrm = load_attr(GX_VA_NRM, vidx).xyz;
    }
    let nrm_tmp = vec4f(nrm, 0.0) * mat3x4_at(F_NRM + pnmtx * 3u);
    let mv_nrm = select(nrm_tmp, normalize(nrm_tmp), dot(nrm_tmp, nrm_tmp) > 1e-10);

    let hdr = ubuf.u[U_HDR0];
    let tex_mask = hdr.w & 0xFFu;
    let chan_mask = (hdr.w >> 8u) & 3u;

    var clr0 = vec4f(0.0);
    var clr1 = vec4f(0.0);
    if (attr_desc(GX_VA_CLR0).atype != 0u) { clr0 = load_color(GX_VA_CLR0, vidx); }
    if (attr_desc(GX_VA_CLR0 + 1u).atype != 0u) { clr1 = load_color(GX_VA_CLR0 + 1u, vidx); }

    if ((chan_mask & 1u) != 0u) {
        var c = light_channel(0u, false, mv_pos, mv_nrm, clr0);
        c.a = light_channel(0u, true, mv_pos, mv_nrm, clr0).a;
        out.cc0 = c;
    }
    if ((chan_mask & 2u) != 0u) {
        var c = light_channel(1u, false, mv_pos, mv_nrm, clr1);
        c.a = light_channel(1u, true, mv_pos, mv_nrm, clr1).a;
        out.cc1 = c;
    }

    out.tex0 = texgen_one(0u, tex_mask, pos, nrm, clr0, clr1, vidx);
    out.tex1 = texgen_one(1u, tex_mask, pos, nrm, clr0, clr1, vidx);
    out.tex2 = texgen_one(2u, tex_mask, pos, nrm, clr0, clr1, vidx);
    out.tex3 = texgen_one(3u, tex_mask, pos, nrm, clr0, clr1, vidx);
    out.tex4 = texgen_one(4u, tex_mask, pos, nrm, clr0, clr1, vidx);
    out.tex5 = texgen_one(5u, tex_mask, pos, nrm, clr0, clr1, vidx);
    out.tex6 = texgen_one(6u, tex_mask, pos, nrm, clr0, clr1, vidx);
    out.tex7 = texgen_one(7u, tex_mask, pos, nrm, clr0, clr1, vidx);
    return out;
}

// ---- Fragment stage -----------------------------------------------------

// Component select without dynamic vector indexing.
fn comp(v: vec4f, i: u32) -> f32 {
    return select(select(select(v.x, v.y, i == 1u), v.z, i == 2u), v.w, i == 3u);
}

fn swz(v: vec4f, s: u32) -> vec4f {
    return vec4f(comp(v, s & 3u), comp(v, (s >> 2u) & 3u), comp(v, (s >> 4u) & 3u), comp(v, (s >> 6u) & 3u));
}

struct Regs {
    r0: vec4f,
    r1: vec4f,
    r2: vec4f,
    r3: vec4f,
};

fn reg_get(r: Regs, i: u32) -> vec4f {
    return select(select(select(r.r0, r.r1, i == 1u), r.r2, i == 2u), r.r3, i == 3u);
}

fn reg_set_rgb(r: Regs, i: u32, c: vec3f) -> Regs {
    var o = r;
    o.r0 = select(o.r0, vec4f(c, o.r0.a), i == 0u);
    o.r1 = select(o.r1, vec4f(c, o.r1.a), i == 1u);
    o.r2 = select(o.r2, vec4f(c, o.r2.a), i == 2u);
    o.r3 = select(o.r3, vec4f(c, o.r3.a), i == 3u);
    return o;
}

fn reg_set_a(r: Regs, i: u32, a: f32) -> Regs {
    var o = r;
    o.r0.a = select(o.r0.a, a, i == 0u);
    o.r1.a = select(o.r1.a, a, i == 1u);
    o.r2.a = select(o.r2.a, a, i == 2u);
    o.r3.a = select(o.r3.a, a, i == 3u);
    return o;
}

fn pick_vec3(i: u32, a: vec3f, b: vec3f, c: vec3f, d: vec3f, e: vec3f, f: vec3f, g: vec3f, h: vec3f) -> vec3f {
    return select(select(select(select(select(select(select(a, b, i == 1u), c, i == 2u), d, i == 3u), e, i == 4u),
                                f, i == 5u), g, i == 6u), h, i == 7u);
}

fn sample_tex(m: u32, uv: vec2f) -> vec4f {
    let bias = ubuf.f[F_TEXSB + m].z;
    switch m {
        case 0u: {
            if (textureNumLevels(tex0) == 1u) { return textureSampleLevel(tex0, tex0_samp, uv, 0.0); }
            return textureSampleBias(tex0, tex0_samp, uv, bias);
        }
        case 1u: {
            if (textureNumLevels(tex1) == 1u) { return textureSampleLevel(tex1, tex1_samp, uv, 0.0); }
            return textureSampleBias(tex1, tex1_samp, uv, bias);
        }
        case 2u: {
            if (textureNumLevels(tex2) == 1u) { return textureSampleLevel(tex2, tex2_samp, uv, 0.0); }
            return textureSampleBias(tex2, tex2_samp, uv, bias);
        }
        case 3u: {
            if (textureNumLevels(tex3) == 1u) { return textureSampleLevel(tex3, tex3_samp, uv, 0.0); }
            return textureSampleBias(tex3, tex3_samp, uv, bias);
        }
        case 4u: {
            if (textureNumLevels(tex4) == 1u) { return textureSampleLevel(tex4, tex4_samp, uv, 0.0); }
            return textureSampleBias(tex4, tex4_samp, uv, bias);
        }
        case 5u: {
            if (textureNumLevels(tex5) == 1u) { return textureSampleLevel(tex5, tex5_samp, uv, 0.0); }
            return textureSampleBias(tex5, tex5_samp, uv, bias);
        }
        case 6u: {
            if (textureNumLevels(tex6) == 1u) { return textureSampleLevel(tex6, tex6_samp, uv, 0.0); }
            return textureSampleBias(tex6, tex6_samp, uv, bias);
        }
        default: {
            if (textureNumLevels(tex7) == 1u) { return textureSampleLevel(tex7, tex7_samp, uv, 0.0); }
            return textureSampleBias(tex7, tex7_samp, uv, bias);
        }
    }
}

fn konst_color(sel: u32) -> vec3f {
    if (sel < 8u) { return vec3f(f32(8u - sel) / 8.0); }
    if (sel >= 12u && sel <= 15u) { return uf(F_KCOLOR + (sel - 12u)).rgb; }
    if (sel >= 16u) { return vec3f(comp(uf(F_KCOLOR + (sel & 3u)), (sel - 16u) >> 2u)); }
    return vec3f(1.0);
}

fn konst_alpha(sel: u32) -> f32 {
    if (sel < 8u) { return f32(8u - sel) / 8.0; }
    if (sel >= 16u) { return comp(uf(F_KCOLOR + (sel & 3u)), (sel - 16u) >> 2u); }
    return 1.0;
}

struct StageIn {
    has_tex: bool,
    sampled: vec4f,
    swap_tex: u32,
    has_rast: bool,
    rast: vec4f,
    swap_ras: u32,
    ksel: u32,
    asel: u32,
};

fn color_arg(arg: u32, regs: Regs, s: StageIn) -> vec3f {
    switch arg {
        case 0u: { return regs.r0.rgb; }
        case 1u: { return vec3f(regs.r0.a); }
        case 2u: { return regs.r1.rgb; }
        case 3u: { return vec3f(regs.r1.a); }
        case 4u: { return regs.r2.rgb; }
        case 5u: { return vec3f(regs.r2.a); }
        case 6u: { return regs.r3.rgb; }
        case 7u: { return vec3f(regs.r3.a); }
        case 8u: {
            if (!s.has_tex) { return vec3f(1.0); }
            return swz(s.sampled, s.swap_tex).rgb;
        }
        case 9u: {
            if (!s.has_tex) { return vec3f(1.0); }
            return vec3f(swz(s.sampled, s.swap_tex).a);
        }
        case 10u: {
            if (!s.has_rast) { return vec3f(0.0); }
            return swz(s.rast, s.swap_ras).rgb;
        }
        case 11u: {
            if (!s.has_rast) { return vec3f(0.0); }
            return vec3f(swz(s.rast, s.swap_ras).a);
        }
        case 12u: { return vec3f(1.0); }
        case 13u: { return vec3f(0.5); }
        case 14u: { return konst_color(s.ksel); }
        default: { return vec3f(0.0); }
    }
}

fn alpha_arg(arg: u32, regs: Regs, s: StageIn) -> f32 {
    switch arg {
        case 0u: { return regs.r0.a; }
        case 1u: { return regs.r1.a; }
        case 2u: { return regs.r2.a; }
        case 3u: { return regs.r3.a; }
        case 4u: {
            if (!s.has_tex) { return 1.0; }
            return swz(s.sampled, s.swap_tex).a;
        }
        case 5u: {
            if (!s.has_rast) { return 0.0; }
            return swz(s.rast, s.swap_ras).a;
        }
        case 6u: { return konst_alpha(s.asel); }
        default: { return 0.0; }
    }
}

fn color_arg_normalized(arg: u32, cmask: u32, amask: u32) -> bool {
    switch arg {
        case 0u: { return (cmask & 1u) != 0u; }
        case 1u: { return (amask & 1u) != 0u; }
        case 2u: { return (cmask & 2u) != 0u; }
        case 3u: { return (amask & 2u) != 0u; }
        case 4u: { return (cmask & 4u) != 0u; }
        case 5u: { return (amask & 4u) != 0u; }
        case 6u: { return (cmask & 8u) != 0u; }
        case 7u: { return (amask & 8u) != 0u; }
        default: { return true; }
    }
}

fn alpha_arg_normalized(arg: u32, amask: u32) -> bool {
    switch arg {
        case 0u: { return (amask & 1u) != 0u; }
        case 1u: { return (amask & 2u) != 0u; }
        case 2u: { return (amask & 4u) != 0u; }
        case 3u: { return (amask & 8u) != 0u; }
        default: { return true; }
    }
}

fn tev_scale_bias3(v: vec3f, bias: u32, scale: u32) -> vec3f {
    var r = v;
    if (bias == 1u) { r = r + 0.5; } else if (bias == 2u) { r = r - 0.5; }
    if (scale == 1u) { r = r * 2.0; } else if (scale == 2u) { r = r * 4.0; } else if (scale == 3u) { r = r / 2.0; }
    return r;
}

fn tev_scale_bias1(v: f32, bias: u32, scale: u32) -> f32 {
    var r = v;
    if (bias == 1u) { r = r + 0.5; } else if (bias == 2u) { r = r - 0.5; }
    if (scale == 1u) { r = r * 2.0; } else if (scale == 2u) { r = r * 4.0; } else if (scale == 3u) { r = r / 2.0; }
    return r;
}

fn tev_color_op(op: u32, bias: u32, scale: u32, clamped: bool, a: vec3f, b: vec3f, c: vec3f, d: vec3f) -> vec3f {
    var r: vec3f;
    if (op == 0u || op == 1u) {
        var m = mix(a, b, c);
        if (op == 1u) { m = -m; }
        r = tev_scale_bias3(m + d, bias, scale);
    } else {
        let zero = vec3f(0.0);
        switch op {
            case 8u: { r = select(zero, c, round(a.r * 255.0) > round(b.r * 255.0)) + d; }
            case 9u: { r = select(zero, c, round(a.r * 255.0) == round(b.r * 255.0)) + d; }
            case 10u: {
                r = select(zero, c, round(dot(a.rg * 255.0, vec2f(1.0, 256.0))) >
                                    round(dot(b.rg * 255.0, vec2f(1.0, 256.0)))) + d;
            }
            case 11u: {
                r = select(zero, c, round(dot(a.rg * 255.0, vec2f(1.0, 256.0))) ==
                                    round(dot(b.rg * 255.0, vec2f(1.0, 256.0)))) + d;
            }
            case 12u: {
                r = select(zero, c, round(dot(a * 255.0, vec3f(1.0, 256.0, 65536.0))) >
                                    round(dot(b * 255.0, vec3f(1.0, 256.0, 65536.0)))) + d;
            }
            case 13u: {
                r = select(zero, c, round(dot(a * 255.0, vec3f(1.0, 256.0, 65536.0))) ==
                                    round(dot(b * 255.0, vec3f(1.0, 256.0, 65536.0)))) + d;
            }
            case 14u: { r = select(zero, c, round(a * 255.0) > round(b * 255.0)) + d; }
            default: { r = select(zero, c, round(a * 255.0) == round(b * 255.0)) + d; }
        }
    }
    if (clamped) { return clamp(r, vec3f(0.0), vec3f(1.0)); }
    return clamp(r, vec3f(-4.0), vec3f(4.0));
}

fn tev_alpha_op(op: u32, bias: u32, scale: u32, clamped: bool, a: f32, b: f32, c: f32, d: f32) -> f32 {
    var r: f32;
    if (op == 0u || op == 1u) {
        var m = mix(a, b, c);
        if (op == 1u) { m = -m; }
        r = tev_scale_bias1(m + d, bias, scale);
    } else if (op == 14u) {
        r = select(0.0, c, round(a * 255.0) > round(b * 255.0)) + d;
    } else {
        r = select(0.0, c, round(a * 255.0) == round(b * 255.0)) + d;
    }
    if (clamped) { return clamp(r, 0.0, 1.0); }
    return clamp(r, -4.0, 4.0);
}

fn compare_alpha(comp: u32, alpha: u32, ref_value: u32) -> bool {
    switch comp {
        case 0u: { return false; }
        case 1u: { return alpha < ref_value; }
        case 2u: { return alpha == ref_value; }
        case 3u: { return alpha <= ref_value; }
        case 4u: { return alpha > ref_value; }
        case 5u: { return alpha != ref_value; }
        case 6u: { return alpha >= ref_value; }
        default: { return true; }
    }
}

@fragment
fn fs_main(in: VertexOutput) -> @location(0) vec4f {
    let hdr = ubuf.u[U_HDR0];
    let stage_count = hdr.y;
    var regs = Regs(uf(F_TEVREG), uf(F_TEVREG + 1u), uf(F_TEVREG + 2u), uf(F_TEVREG + 3u));
    var cmask = 0u;
    var amask = 0u;
    var last_cout = 0u;
    var last_aout = 0u;
    for (var s = 0u; s < stage_count; s++) {
        let w = ubuf.u[U_TEV + s];
        let ca = w.x & 15u;
        let cb = (w.x >> 4u) & 15u;
        let cc = (w.x >> 8u) & 15u;
        let cd = (w.x >> 12u) & 15u;
        let aa = (w.x >> 16u) & 7u;
        let ab = (w.x >> 19u) & 7u;
        let ac = (w.x >> 22u) & 7u;
        let ad = (w.x >> 25u) & 7u;
        let cop = w.y & 15u;
        let cbias = (w.y >> 4u) & 3u;
        let cscale = (w.y >> 6u) & 3u;
        let cout = (w.y >> 8u) & 3u;
        let cclamp = ((w.y >> 10u) & 1u) != 0u;
        let aop = (w.y >> 16u) & 15u;
        let abias = (w.y >> 20u) & 3u;
        let ascale = (w.y >> 22u) & 3u;
        let aout = (w.y >> 24u) & 3u;
        let aclamp = ((w.y >> 26u) & 1u) != 0u;
        let ksel = w.z & 255u;
        let asel = (w.z >> 8u) & 255u;
        let tex_coord = (w.z >> 16u) & 15u;
        let tex_map = (w.z >> 20u) & 15u;
        let channel = (w.z >> 24u) & 15u;
        let swap_ras = (w.z >> 28u) & 3u;
        let swap_tex = (w.z >> 30u) & 3u;

        var st: StageIn;
        st.has_tex = tex_map != 15u;
        st.swap_tex = pick4u(ubuf.u[U_SWAP], swap_tex);
        st.swap_ras = pick4u(ubuf.u[U_SWAP], swap_ras);
        st.ksel = ksel;
        st.asel = asel;
        st.has_rast = channel != CH_ZERO && channel != 15u;
        if (st.has_rast) {
            let ridx = select(0u, 1u, channel == 1u || channel == 3u || channel == 5u);
            st.rast = select(in.cc0, in.cc1, ridx == 1u);
        }
        let uses_tex = tex_map != 15u &&
            (ca == 8u || ca == 9u || cb == 8u || cb == 9u || cc == 8u || cc == 9u || cd == 8u || cd == 9u ||
             aa == 4u || ab == 4u || ac == 4u || ad == 4u);
        if (uses_tex) {
            let tcv = pick_vec3(tex_coord, in.tex0, in.tex1, in.tex2, in.tex3, in.tex4, in.tex5, in.tex6, in.tex7);
            var uv = tcv.xy;
            if (ubuf.u[U_TCG + tex_coord].x == TG_MTX3x4) {
                let near_zero = abs(tcv.z) < 1e-6;
                let q = select(tcv.z, 1.0, near_zero);
                uv = select(tcv.xy / q, tcv.xy, near_zero);
            }
            st.sampled = sample_tex(tex_map, uv);
        }

        let c_a = color_arg(ca, regs, st);
        let c_b = color_arg(cb, regs, st);
        let c_c = color_arg(cc, regs, st);
        let c_d = color_arg(cd, regs, st);
        let na = color_arg_normalized(ca, cmask, amask);
        let nb = color_arg_normalized(cb, cmask, amask);
        let nc = color_arg_normalized(cc, cmask, amask);
        let xa = select(tev_overflow_vec3f(c_a), c_a, na);
        let xb = select(tev_overflow_vec3f(c_b), c_b, nb);
        let xc = select(tev_overflow_vec3f(c_c), c_c, nc);
        let color = tev_color_op(cop, cbias, cscale, cclamp, xa, xb, xc, c_d);
        regs = reg_set_rgb(regs, cout, color);
        cmask = (cmask & ~(1u << cout)) | (select(0u, 1u, cclamp) << cout);

        let a_a = alpha_arg(aa, regs, st);
        let a_b = alpha_arg(ab, regs, st);
        let a_c = alpha_arg(ac, regs, st);
        let a_d = alpha_arg(ad, regs, st);
        let ya = select(tev_overflow_f32(a_a), a_a, alpha_arg_normalized(aa, amask));
        let yb = select(tev_overflow_f32(a_b), a_b, alpha_arg_normalized(ab, amask));
        let yc = select(tev_overflow_f32(a_c), a_c, alpha_arg_normalized(ac, amask));
        regs = reg_set_a(regs, aout, tev_alpha_op(aop, abias, ascale, aclamp, ya, yb, yc, a_d));
        amask = (amask & ~(1u << aout)) | (select(0u, 1u, aclamp) << aout);
        last_cout = cout;
        last_aout = aout;
    }

    var prev = regs.r0;
    if (last_cout != 0u) { prev = vec4f(reg_get(regs, last_cout).rgb, prev.a); }
    if (last_aout != 0u) { prev.a = reg_get(regs, last_aout).a; }

    let fog_type = hdr.z & 0xFFu;
    if (fog_type != 0u) {
        let fa = uf(F_FOG + 1u);
        let depth = 1.0 - in.pos.z;
        var fog_base: f32;
        if ((fog_type & 8u) != 0u) {
            fog_base = fa.x * depth;
        } else {
            fog_base = fa.x / (fa.y - depth);
        }
        if (((hdr.z >> 8u) & 1u) != 0u) {
            fog_base = fog_base * bitcast<f32>(abuf[imm.fog_range_base + u32(in.pos.x)]);
        }
        var fog_f = clamp(fog_base - fa.z, 0.0, 1.0);
        var fog_z = fog_f;
        switch (fog_type & 7u) {
            case 4u: { fog_z = 1.0 - exp2(-8.0 * fog_f); }
            case 5u: { fog_z = 1.0 - exp2(-8.0 * fog_f * fog_f); }
            case 6u: { fog_z = exp2(-8.0 * (1.0 - fog_f)); }
            case 7u: {
                fog_f = 1.0 - fog_f;
                fog_z = exp2(-8.0 * fog_f * fog_f);
            }
            default: {}
        }
        prev = vec4f(mix(prev.rgb, uf(F_FOG).rgb, clamp(fog_z, 0.0, 1.0)), prev.a);
    }

    let prev_color_normalized = (cmask & (1u << last_cout)) != 0u;
    let prev_alpha_normalized = (amask & (1u << last_aout)) != 0u;
    if (!prev_color_normalized && !prev_alpha_normalized) {
        prev = tev_overflow_vec4f(prev);
    } else if (!prev_color_normalized) {
        prev = vec4f(tev_overflow_vec3f(prev.rgb), prev.a);
    } else if (!prev_alpha_normalized) {
        prev.a = tev_overflow_f32(prev.a);
    }

    if (ubuf.u[U_HDR2].y != 0u) {
        let alpha_value = u32(round(clamp(prev.a, 0.0, 1.0) * 255.0));
        let h1 = ubuf.u[U_HDR1];
        let c0 = compare_alpha(h1.x, alpha_value, h1.y);
        let c1 = compare_alpha(h1.z, alpha_value, h1.w);
        var passed: bool;
        switch ubuf.u[U_HDR2].x {
            case 0u: { passed = c0 && c1; }
            case 1u: { passed = c0 || c1; }
            case 2u: { passed = c0 != c1; }
            default: { passed = c0 == c1; }
        }
        if (!passed) { discard; }
    }
    return prev;
}
)WGSL";

    std::string text = consts;
    text += "\n";
    text += helpers;
    text += "\n";
    text += replace_all(replace_all(std::string(body), "@F_COUNT@", std::to_string(F_COUNT)), "@U_COUNT@",
                        std::to_string(U_COUNT));
#if defined(AURORA_GFX_DEKO3D)
    // Tint's GLSL writer cannot lower textureNumLevels here; deko3d always takes the bias sample, as
    // shader.cpp's specialised shaders do (legacyTexLod).
    for (int i = 0; i < 8; ++i)
        text = replace_all(text, "textureNumLevels(tex" + std::to_string(i) + ") == 1u", "false");
#endif
    return text;
}

} // namespace melee_nx::uber
