// SPDX-License-Identifier: GPL-3.0-or-later
// SPIR-V rewrites (declarations in nvn_spirv_patch.h).
#include "nvn_spirv_patch.h"

#include <cstring>
#include <map>

namespace nxrt::nvn::be::gpu {
namespace {

enum : uint32_t {
    OpEntryPoint = 15, OpTypeVoid = 19, OpTypeInt = 21, OpTypeFloat = 22, OpTypeVector = 23, OpTypePointer = 32, OpTypeForwardPointer = 39,
    OpFunction = 54, OpVariable = 59, OpDecorate = 71,
    DecorationLocation = 30, StorageOutput = 3,
};

struct Inst {
    size_t at;
    uint32_t op, len;
};

bool is_type_or_constant(uint32_t op) { return (op >= OpTypeVoid && op <= OpTypeForwardPointer) || (op >= 41 && op <= 52); }

}  // namespace

bool spirv_add_outputs(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out) {
    if (slots.empty() || in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }

    uint32_t bound = in[3];
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ints;    // (width, signed) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs;    // (component type, count) -> id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrs;    // (storage class, pointee) -> id
    std::map<uint32_t, uint32_t> output_location;              // variable id -> Location
    std::map<uint32_t, uint32_t> output_vars;                  // output variable id -> pointee type id
    uint32_t f32 = 0;
    size_t first_type = 0, first_function = 0, entry_at = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (!first_function && n.op == OpFunction) first_function = n.at;
        if (n.op == OpEntryPoint && !entry_at) entry_at = n.at;
        if (n.op == OpTypeInt && n.len == 4) ints[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) output_location[w[1]] = w[3];
    }
    if (!first_type || !first_function || !entry_at || !f32) return false;
    for (const Inst& n : insts)
        if (n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput) output_vars[in[n.at + 2]] = in[n.at + 1];
    for (const SpvOutputSlot& s : slots)
        for (const auto& kv : output_location)
            if (output_vars.count(kv.first) && kv.second == s.location) return false;   // already declared (as a single variable)

    std::vector<uint32_t> decos, decls, new_vars;
    auto scalar_id = [&](SpvScalar t) -> uint32_t {
        if (t == SpvScalar::Float) return f32;
        const std::pair<uint32_t, uint32_t> k = {32, t == SpvScalar::Sint ? 1u : 0u};
        auto it = ints.find(k);
        if (it != ints.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypeInt, id, 32u, k.second});
        ints[k] = id;
        return id;
    };
    auto vec4_id = [&](uint32_t scalar) -> uint32_t {
        const std::pair<uint32_t, uint32_t> k = {scalar, 4};
        auto it = vecs.find(k);
        if (it != vecs.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypeVector, id, scalar, 4u});
        vecs[k] = id;
        return id;
    };
    auto out_ptr_id = [&](uint32_t pointee) -> uint32_t {
        const std::pair<uint32_t, uint32_t> k = {StorageOutput, pointee};
        auto it = ptrs.find(k);
        if (it != ptrs.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypePointer, id, StorageOutput, pointee});
        ptrs[k] = id;
        return id;
    };
    for (const SpvOutputSlot& s : slots) {
        const uint32_t ptr = out_ptr_id(vec4_id(scalar_id(s.type)));
        const uint32_t var = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpVariable, ptr, var, StorageOutput});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, var, DecorationLocation, s.location});
        new_vars.push_back(var);
    }

    out.clear();
    out.reserve(in.size() + decos.size() + decls.size() + new_vars.size() + 4);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    out[3] = bound;
    const Inst* entry = nullptr;
    for (const Inst& n : insts)
        if (n.at == entry_at) entry = &n;
    for (const Inst& n : insts) {
        if (n.at == first_type) out.insert(out.end(), decos.begin(), decos.end());
        if (n.at == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (&n == entry) {
            // the interface list is the tail of OpEntryPoint
            out.push_back(((n.len + static_cast<uint32_t>(new_vars.size())) << 16) | OpEntryPoint);
            out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at) + 1, in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
            out.insert(out.end(), new_vars.begin(), new_vars.end());
            continue;
        }
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_dual_source_output(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, bool> is_output;
    for (const Inst& n : insts)
        if (n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput) is_output[in[n.at + 2]] = true;
    std::vector<uint32_t> result;
    result.reserve(in.size() + 4);
    result.insert(result.end(), in.begin(), in.begin() + 5);
    bool changed = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpDecorate && n.len == 4 && w[2] == DecorationLocation && w[3] == 1 && is_output.count(w[1])) {
            result.insert(result.end(), {(4u << 16) | OpDecorate, w[1], DecorationLocation, 0u});
            result.insert(result.end(), {(4u << 16) | OpDecorate, w[1], 32u /* Index */, 1u});
            changed = true;
            continue;
        }
        result.insert(result.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    if (!changed) return false;
    out = std::move(result);
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {
namespace {
enum : uint32_t { OpCapability = 17, OpExecutionMode = 16, OpConstant = 43, OpStore = 62, OpLoad = 61, OpCopyObject = 83, OpAccessChain = 65, OpInBoundsAccessChain = 66,
                  DecOffset = 35, DecXfbBuffer = 36, DecXfbStride = 37, CapTransformFeedback = 53, ExecXfb = 11 };
}  // namespace

bool spirv_add_xfb(const std::vector<uint32_t>& in, std::vector<uint32_t>& out, std::vector<SpvXfbOutput>* layout, uint32_t* stride) {
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    uint32_t f32 = 0, entry_id = 0;
    size_t first_type = 0, after_entry = 0, first_cap_end = 0;
    std::map<uint32_t, uint32_t> location;                            // variable id -> Location
    std::map<uint32_t, uint32_t> output_ptr_type;                     // output variable id -> pointer type id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vecs, ptrs;     // (component, count) / (storage, pointee) -> id
    std::map<uint32_t, uint32_t> constants;                           // OpConstant id -> low word
    std::map<uint32_t, uint32_t> max_component;                       // variable -> highest constant component index stored (+1)
    std::map<uint32_t, bool> whole;                                   // variable used as a whole (store, load, copy)
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!first_type && is_type_or_constant(n.op)) first_type = n.at;
        if (n.op == OpCapability) first_cap_end = n.at + n.len;
        if (n.op == OpEntryPoint && !entry_id) entry_id = w[2];
        if (n.op == OpEntryPoint || n.op == OpExecutionMode) after_entry = n.at + n.len;
        if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4) vecs[{w[2], w[3]}] = w[1];
        else if (n.op == OpTypePointer && n.len == 4) ptrs[{w[2], w[3]}] = w[1];
        else if (n.op == OpConstant && n.len >= 4) constants[w[2]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariable && n.len >= 4 && w[3] == StorageOutput) output_ptr_type[w[2]] = w[1];
    }
    if (!first_type || !first_cap_end || !after_entry || !f32 || !entry_id) return false;
    auto is_captured = [&](uint32_t v) { return location.count(v) && output_ptr_type.count(v); };
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChain || n.op == OpInBoundsAccessChain) && n.len >= 5) {
            const uint32_t base = w[3];
            if (!is_captured(base)) continue;
            auto c = constants.find(w[4]);
            if (c == constants.end()) whole[base] = true;
            else max_component[base] = std::max(max_component[base], c->second + 1);
        } else if (n.op == OpStore && n.len >= 3) {
            if (is_captured(w[1])) whole[w[1]] = true;
        } else if ((n.op == OpLoad || n.op == OpCopyObject) && n.len >= 4) {
            if (is_captured(w[3])) whole[w[3]] = true;
        }
    }
    // the captured variables in Location order, each narrowed to the components that are stored
    std::map<uint32_t, uint32_t> by_location;   // Location -> variable id
    for (const auto& kv : output_ptr_type)
        if (location.count(kv.first)) by_location[location[kv.first]] = kv.first;
    if (by_location.empty()) return false;
    std::vector<uint32_t> decos, decls;
    std::map<uint32_t, uint32_t> new_ptr;       // variable id -> replacement pointer type
    uint32_t offset = 0;
    std::vector<SpvXfbOutput> lay;
    std::vector<std::pair<uint32_t, uint32_t>> decorate;   // variable, byte offset
    for (const auto& kv : by_location) {
        const uint32_t var = kv.second;
        uint32_t ncomp = (whole.count(var) || !max_component.count(var)) ? 4u : std::min(4u, max_component[var]);
        if (ncomp < 2) ncomp = 4;                // a lone scalar component keeps its vec4 (a scalar cannot be indexed)
        if (ncomp != 4) {
            const std::pair<uint32_t, uint32_t> vk = {f32, ncomp};
            uint32_t vid;
            auto vit = vecs.find(vk);
            if (vit != vecs.end()) {
                vid = vit->second;
            } else {
                vid = bound++;
                decls.insert(decls.end(), {(4u << 16) | OpTypeVector, vid, f32, ncomp});
                vecs[vk] = vid;
            }
            const std::pair<uint32_t, uint32_t> pk = {StorageOutput, vid};
            uint32_t pid;
            auto pit = ptrs.find(pk);
            if (pit != ptrs.end()) {
                pid = pit->second;
            } else {
                pid = bound++;
                decls.insert(decls.end(), {(4u << 16) | OpTypePointer, pid, StorageOutput, vid});
                ptrs[pk] = pid;
            }
            new_ptr[var] = pid;
        }
        decorate.push_back({var, offset});
        lay.push_back({kv.first, ncomp, offset});
        offset += 4 * ncomp;
    }
    for (const auto& d : decorate) {
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecXfbBuffer, 0u});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecXfbStride, offset});
        decos.insert(decos.end(), {(4u << 16) | OpDecorate, d.first, DecOffset, d.second});
    }
    std::vector<uint32_t> res;
    res.reserve(in.size() + decos.size() + decls.size() + 8);
    res.insert(res.end(), in.begin(), in.begin() + 5);
    res[3] = bound;
    bool decls_done = false;
    for (const Inst& n : insts) {
        if (n.at == first_type) res.insert(res.end(), decos.begin(), decos.end());
        // the new vector and pointer types go in front of the first variable that uses one (every type precedes its users)
        if (!decls_done && ((n.op == OpVariable && n.len >= 4 && in[n.at + 3] == StorageOutput && new_ptr.count(in[n.at + 2])) || n.op == OpFunction)) {
            res.insert(res.end(), decls.begin(), decls.end());
            decls_done = true;
        }
        std::vector<uint32_t> copy(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpVariable && copy.size() >= 4 && copy[3] == StorageOutput) {
            auto it = new_ptr.find(copy[2]);
            if (it != new_ptr.end()) copy[1] = it->second;
        }
        res.insert(res.end(), copy.begin(), copy.end());
        if (n.at + n.len == first_cap_end) res.insert(res.end(), {(2u << 16) | OpCapability, CapTransformFeedback});
        if (n.at + n.len == after_entry) res.insert(res.end(), {(3u << 16) | OpExecutionMode, entry_id, ExecXfb});
    }
    if (!decls_done) return false;
    out = std::move(res);
    if (layout) *layout = lay;
    if (stride) *stride = offset;
    return true;
}

bool spirv_outputs_to_private(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    if (in.size() < 5) return false;
    enum : uint32_t { OpVariableOp = 59, OpDecorateOp = 71, OpEntryPointOp = 15, StoragePrivate = 6, DecIndex = 32, DecComponent = 31, DecBuiltIn = 11 };
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    std::map<uint32_t, bool> out_var, has_location, builtin;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpVariableOp && n.len >= 4 && w[3] == StorageOutput) out_var[w[2]] = true;
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecorationLocation) has_location[w[1]] = true;
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecBuiltIn) builtin[w[1]] = true;
    }
    if (out_var.empty()) return false;
    for (const auto& kv : out_var)
        if (builtin.count(kv.first) || !has_location.count(kv.first)) return false;
    const bool before_14 = in[1] < 0x10400;
    std::vector<uint32_t> res(in.begin(), in.begin() + 5);
    res.reserve(in.size());
    for (const Inst& n : insts) {
        std::vector<uint32_t> c(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpTypePointer && n.len == 4 && c[2] == StorageOutput) c[2] = StoragePrivate;
        else if (n.op == OpVariableOp && n.len >= 4 && c[3] == StorageOutput) c[3] = StoragePrivate;
        else if (n.op == OpDecorateOp && n.len >= 4 && out_var.count(c[1]) && (c[2] == DecorationLocation || c[2] == DecIndex || c[2] == DecComponent)) continue;
        else if (n.op == OpEntryPointOp && before_14) {
            // OpEntryPoint model id name... interface ids: drop the output variables from the interface
            size_t name_words = 0;
            const char* nm = reinterpret_cast<const char*>(&c[3]);
            const size_t max_chars = (c.size() - 3) * 4;
            size_t chars = 0;
            while (chars < max_chars && nm[chars]) ++chars;
            name_words = chars / 4 + 1;
            const size_t iface = 3 + name_words;
            std::vector<uint32_t> d(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(std::min(iface, c.size())));
            for (size_t i = iface; i < c.size(); ++i)
                if (!out_var.count(c[i])) d.push_back(c[i]);
            d[0] = (static_cast<uint32_t>(d.size()) << 16) | OpEntryPointOp;
            c = d;
        }
        res.insert(res.end(), c.begin(), c.end());
    }
    out = std::move(res);
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_gs_passthrough(const std::vector<uint32_t>& in, const std::vector<SpvOutputSlot>& slots, std::vector<uint32_t>& out) {
    enum : uint32_t { OpExecutionModeOp = 16, OpConstantOp = 43, OpLoadOp = 61, OpStoreOp = 62, OpAccessChainOp = 65, OpEmitVertexOp = 218,
                      OpTypeArrayOp = 28, StorageInput = 1, ExecInputTriangles = 22 };
    for (const SpvOutputSlot& s : slots)
        if (s.type != SpvScalar::Float) return false;
    std::vector<uint32_t> tmp;
    if (!spirv_add_outputs(in, slots, tmp)) return false;       // the outputs (never written yet) and the interface entries

    std::vector<Inst> insts;
    for (size_t i = 5; i < tmp.size();) {
        const uint32_t len = tmp[i] >> 16;
        if (!len || i + len > tmp.size()) return false;
        insts.push_back({i, tmp[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = tmp[3];
    uint32_t f32 = 0, int32 = 0, v4 = 0, ptr_in_v4 = 0, first_function = 0, input_triangles = 0;
    std::map<uint32_t, uint32_t> location, var_type, array_elem, array_len_id, int_const;   // var -> Location / pointer type; array type -> element / length id; value -> id
    std::map<uint32_t, uint32_t> ptr_pointee, const_value;
    std::map<uint32_t, uint32_t> input_var, output_var;                                     // Location -> variable id
    for (const Inst& n : insts) {
        const uint32_t* w = &tmp[n.at];
        if (!first_function && n.op == OpFunction) first_function = static_cast<uint32_t>(n.at);
        if (n.op == OpExecutionModeOp && n.len >= 3 && w[2] == ExecInputTriangles) input_triangles = 1;
        if (n.op == OpTypeFloat && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeInt && n.len == 4 && w[2] == 32 && w[3] == 1) int32 = w[1];
        else if (n.op == OpTypeVector && n.len == 4 && w[3] == 4 && f32 && w[2] == f32) v4 = w[1];
        else if (n.op == OpTypePointer && n.len == 4) { ptr_pointee[w[1]] = w[3]; if (w[2] == StorageInput && w[3] == v4) ptr_in_v4 = w[1]; }
        else if (n.op == OpTypeArrayOp && n.len == 4) { array_elem[w[1]] = w[2]; array_len_id[w[1]] = w[3]; }
        else if (n.op == OpConstantOp && n.len == 4) { const_value[w[2]] = w[3]; if (w[1] == int32) int_const[w[3]] = w[2]; }
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariable && n.len >= 4) var_type[w[2]] = w[1];
    }
    if (!input_triangles || !f32 || !v4 || !int32 || !first_function) return false;
    for (const Inst& n : insts) {
        const uint32_t* w = &tmp[n.at];
        if (n.op != OpVariable || n.len < 4 || !location.count(w[2])) continue;
        (w[3] == StorageInput ? input_var : output_var)[location[w[2]]] = w[2];
    }
    // every slot: an Input array [3] of vec4 and the Output variable spirv_add_outputs made
    std::vector<std::pair<uint32_t, uint32_t>> copies;   // (input var, output var)
    for (const SpvOutputSlot& s : slots) {
        if (!input_var.count(s.location) || !output_var.count(s.location)) return false;
        const uint32_t ptr = var_type[input_var[s.location]];
        const uint32_t arr = ptr_pointee.count(ptr) ? ptr_pointee[ptr] : 0;
        if (!arr || !array_elem.count(arr) || array_elem[arr] != v4 || !const_value.count(array_len_id[arr]) || const_value[array_len_id[arr]] != 3) return false;
        copies.emplace_back(input_var[s.location], output_var[s.location]);
    }
    uint32_t emits = 0;
    for (const Inst& n : insts) emits += n.op == OpEmitVertexOp;
    if (emits != 3) return false;

    std::vector<uint32_t> decls;
    if (!ptr_in_v4) { ptr_in_v4 = bound++; decls.insert(decls.end(), {(4u << 16) | OpTypePointer, ptr_in_v4, StorageInput, v4}); }
    for (uint32_t k = 0; k < 3; ++k)
        if (!int_const.count(k)) { const uint32_t id = bound++; int_const[k] = id; decls.insert(decls.end(), {(4u << 16) | OpConstantOp, int32, id, k}); }

    out.clear();
    out.reserve(tmp.size() + decls.size() + 12 * copies.size() + 8);
    out.insert(out.end(), tmp.begin(), tmp.begin() + 5);
    uint32_t emit_index = 0;
    for (const Inst& n : insts) {
        if (n.at == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (n.op == OpEmitVertexOp) {
            for (const auto& c : copies) {
                const uint32_t chain = bound++, val = bound++;
                out.insert(out.end(), {(5u << 16) | OpAccessChainOp, ptr_in_v4, chain, c.first, int_const[emit_index]});
                out.insert(out.end(), {(4u << 16) | OpLoadOp, v4, val, chain});
                out.insert(out.end(), {(3u << 16) | OpStoreOp, c.second, val});
            }
            ++emit_index;
        }
        out.insert(out.end(), tmp.begin() + static_cast<std::ptrdiff_t>(n.at), tmp.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
    }
    out[3] = bound;
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_int_attributes(const std::vector<uint32_t>& in, const std::vector<SpvIntSlot>& slots, std::vector<uint32_t>& out) {
    enum : uint32_t { OpTypeIntOp = 21, OpTypeFloatOp = 22, OpTypeVectorOp = 23, OpTypeArrayOp = 28, OpTypePointerOp = 32, OpConstantOp = 43, OpVariableOp = 59,
                      OpLoadOp = 61, OpStoreOp = 62, OpAccessChainOp = 65, OpInBoundsAccessChainOp = 66, OpDecorateOp = 71, OpCompositeConstructOp = 80,
                      OpBitcastOp = 124, OpEntryPointOp = 15, OpFunctionOp = 54, DecComponent = 31, StorageInputC = 1, StoragePrivateC = 6 };
    if (slots.empty() || in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    uint32_t f32 = 0, v4 = 0, u32 = 0, s32 = 0, entry_id = 0;
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptrs;      // (storage, pointee) -> pointer type id
    std::map<uint32_t, uint32_t> ptr_pointee, location, var_type;
    std::map<uint32_t, size_t> type_at;
    std::map<uint32_t, uint32_t> vec4_of;                         // scalar type id -> its vec4 id
    std::map<uint32_t, uint32_t> array_elem, array_len_id, const_value;   // array type -> element type / length constant; constant -> value
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpTypeFloatOp && n.len >= 3 && w[2] == 32) { f32 = w[1]; type_at[w[1]] = n.at; }
        else if (n.op == OpTypeIntOp && n.len == 4 && w[2] == 32) { (w[3] ? s32 : u32) = w[1]; type_at[w[1]] = n.at; }
        else if (n.op == OpTypeVectorOp && n.len == 4 && w[3] == 4) { vec4_of[w[2]] = w[1]; type_at[w[1]] = n.at; }
        else if (n.op == OpTypeArrayOp && n.len == 4) { array_elem[w[1]] = w[2]; array_len_id[w[1]] = w[3]; type_at[w[1]] = n.at; }
        else if (n.op == OpConstantOp && n.len == 4) const_value[w[2]] = w[3];
        else if (n.op == OpTypePointerOp && n.len == 4) { ptrs[{w[2], w[3]}] = w[1]; ptr_pointee[w[1]] = w[3]; type_at[w[1]] = n.at; }
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariableOp && n.len >= 4) var_type[w[2]] = w[1];
        else if (n.op == OpEntryPointOp && !entry_id) entry_id = w[2];
    }
    if (!f32 || !vec4_of.count(f32) || !entry_id) return false;
    v4 = vec4_of[f32];
    // One piece of work per input variable that changes. A plain variable is one vec4 at its Location; the translator also declares an Input
    // array of vec4 (a shader that indexes its attributes with a computed index), which covers Locations base .. base + length - 1.
    struct Elem { uint32_t location; int kind; uint32_t new_var; };   // kind 0 float, 1 uint, 2 sint
    struct Work { uint32_t old_var, old_ptr, array_type; size_t var_at; std::vector<Elem> elems; };
    std::vector<Work> work;
    size_t first_var_at = SIZE_MAX;
    std::map<uint32_t, int> slot_kind;                            // Location -> 1 uint / 2 sint
    for (const SpvIntSlot& sl : slots) slot_kind[sl.location] = sl.is_signed ? 2 : 1;
    std::map<uint32_t, size_t> work_of_var;
    for (const SpvIntSlot& sl : slots) {
        uint32_t var = 0;
        size_t at = 0;
        uint32_t array_type = 0, base = 0, len = 0;
        for (const Inst& n : insts) {
            const uint32_t* w = &in[n.at];
            if (n.op != OpVariableOp || n.len < 4 || w[3] != StorageInputC || !location.count(w[2])) continue;
            const uint32_t loc = location[w[2]], ptr = w[1];
            if (loc == sl.location && ptr_pointee.count(ptr) && ptr_pointee[ptr] == v4) { var = w[2]; at = n.at; array_type = 0; }
            else if (ptr_pointee.count(ptr) && array_elem.count(ptr_pointee[ptr]) && array_elem[ptr_pointee[ptr]] == v4 &&
                     const_value.count(array_len_id[ptr_pointee[ptr]])) {
                const uint32_t n_elems = const_value[array_len_id[ptr_pointee[ptr]]];
                if (sl.location >= loc && sl.location < loc + n_elems) { var = w[2]; at = n.at; array_type = ptr_pointee[ptr]; base = loc; len = n_elems; }
            }
        }
        if (!var) return false;
        if (work_of_var.count(var)) continue;                      // the array of an earlier slot
        Work wk{var, var_type[var], array_type, at, {}};
        if (array_type) {
            for (uint32_t e = 0; e < len; ++e) wk.elems.push_back({base + e, slot_kind.count(base + e) ? slot_kind[base + e] : 0, 0});
        } else {
            wk.elems.push_back({sl.location, sl.is_signed ? 2 : 1, 0});
        }
        work_of_var[var] = work.size();
        work.push_back(std::move(wk));
        first_var_at = std::min(first_var_at, at);
    }
    // the types the new variables need have to be declared before the first variable that changes
    std::vector<uint32_t> types;
    auto need_scalar = [&](bool is_signed) -> uint32_t {
        const uint32_t t = is_signed ? s32 : u32;
        return t && type_at.count(t) && type_at[t] < first_var_at ? t : 0;
    };
    auto vec4_for = [&](uint32_t scalar) -> uint32_t {
        auto it = vec4_of.find(scalar);
        if (it != vec4_of.end() && type_at[it->second] < first_var_at) return it->second;
        const uint32_t id = bound++;
        types.insert(types.end(), {(4u << 16) | OpTypeVectorOp, id, scalar, 4u});
        vec4_of[scalar] = id;
        type_at[id] = 0;
        return id;
    };
    auto ptr_for = [&](uint32_t storage, uint32_t pointee) -> uint32_t {
        auto it = ptrs.find({storage, pointee});
        if (it != ptrs.end() && type_at[it->second] < first_var_at) return it->second;
        const uint32_t id = bound++;
        types.insert(types.end(), {(4u << 16) | OpTypePointerOp, id, storage, pointee});
        ptrs[{storage, pointee}] = id;
        ptr_pointee[id] = pointee;
        type_at[id] = 0;
        return id;
    };
    std::map<uint32_t, uint32_t> priv_ptr_of;    // old Input pointer type -> the Private pointer type to the same pointee
    auto private_of = [&](uint32_t old_ptr) -> uint32_t {
        auto it = priv_ptr_of.find(old_ptr);
        if (it != priv_ptr_of.end()) return it->second;
        return priv_ptr_of[old_ptr] = ptr_for(StoragePrivateC, ptr_pointee[old_ptr]);
    };
    std::map<uint32_t, uint32_t> old_to_work;    // old var -> index in work
    std::vector<uint32_t> decls_after_types, decos;
    uint32_t v4u = 0, v4i = 0;
    for (size_t i = 0; i < work.size(); ++i) {
        Work& wk = work[i];
        for (Elem& el : wk.elems) {
            uint32_t vec = v4;
            if (el.kind) {
                const uint32_t sc = need_scalar(el.kind == 2);
                if (!sc) return false;
                uint32_t& vv = el.kind == 2 ? v4i : v4u;
                if (!vv) vv = vec4_for(sc);
                vec = vv;
            }
            const uint32_t in_ptr = ptr_for(StorageInputC, vec);
            el.new_var = bound++;
            decls_after_types.insert(decls_after_types.end(), {(4u << 16) | OpVariableOp, in_ptr, el.new_var, StorageInputC});
            decos.insert(decos.end(), {(4u << 16) | OpDecorateOp, el.new_var, DecorationLocation, el.location});
        }
        private_of(wk.old_ptr);
        old_to_work[wk.old_var] = static_cast<uint32_t>(i);
    }

    // the pointer types the access chains into the old inputs get (declared before the first changed variable)
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) && n.len >= 5 && old_to_work.count(w[3])) {
            if (!ptr_pointee.count(w[1])) return false;
            private_of(w[1]);
        }
    }

    // the entry function: the new instructions go behind its leading OpVariable run
    uint32_t entry_func = 0;
    for (const Inst& n : insts)
        if (n.op == OpEntryPointOp) { entry_func = in[n.at + 2]; break; }
    size_t insert_at = 0;
    for (size_t k = 0; k < insts.size() && !insert_at; ++k) {
        if (insts[k].op == OpFunctionOp && in[insts[k].at + 2] == entry_func) {
            size_t j = k + 1;
            while (j < insts.size() && insts[j].op != 248 /* OpLabel */) ++j;
            ++j;
            while (j < insts.size() && insts[j].op == OpVariableOp) ++j;
            if (j < insts.size()) insert_at = insts[j].at;
        }
    }
    if (!insert_at) return false;
    std::vector<uint32_t> init;
    for (const Work& wk : work) {
        std::vector<uint32_t> values;
        for (const Elem& el : wk.elems) {
            if (!el.kind) {                                        // a float element: the new input holds the same value
                const uint32_t loaded = bound++;
                init.insert(init.end(), {(4u << 16) | OpLoadOp, v4, loaded, el.new_var});
                values.push_back(loaded);
                continue;
            }
            const uint32_t vec = el.kind == 2 ? v4i : v4u;
            const uint32_t loaded = bound++, cast = bound++;
            init.insert(init.end(), {(4u << 16) | OpLoadOp, vec, loaded, el.new_var});
            init.insert(init.end(), {(4u << 16) | OpBitcastOp, v4, cast, loaded});
            values.push_back(cast);
        }
        if (!wk.array_type) {
            init.insert(init.end(), {(3u << 16) | OpStoreOp, wk.old_var, values[0]});
        } else {
            const uint32_t whole = bound++;
            init.push_back(((3u + static_cast<uint32_t>(values.size())) << 16) | OpCompositeConstructOp);
            init.push_back(wk.array_type);
            init.push_back(whole);
            init.insert(init.end(), values.begin(), values.end());
            init.insert(init.end(), {(3u << 16) | OpStoreOp, wk.old_var, whole});
        }
    }

    out.clear();
    out.reserve(in.size() + types.size() + decls_after_types.size() + decos.size() + init.size() + 16);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    size_t first_function = 0;
    for (const Inst& n : insts)
        if (n.op == OpFunctionOp) { first_function = n.at; break; }
    bool deco_done = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (!deco_done && is_type_or_constant(n.op)) { out.insert(out.end(), decos.begin(), decos.end()); deco_done = true; }
        if (n.at == first_var_at) out.insert(out.end(), types.begin(), types.end());
        if (n.at == first_function) out.insert(out.end(), decls_after_types.begin(), decls_after_types.end());
        if (n.at == insert_at) out.insert(out.end(), init.begin(), init.end());
        std::vector<uint32_t> c(in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        if (n.op == OpVariableOp && n.len >= 4 && old_to_work.count(w[2])) {            // the old input becomes a private variable
            c[1] = private_of(work[old_to_work[w[2]]].old_ptr);
            c[3] = StoragePrivateC;
        } else if (n.op == OpDecorateOp && n.len >= 4 && old_to_work.count(w[1]) && (w[2] == DecorationLocation || w[2] == DecComponent)) {
            continue;
        } else if ((n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) && n.len >= 5 && old_to_work.count(w[3])) {
            c[1] = private_of(w[1]);                                                       // a pointer into the old input is a Private pointer now
        } else if (n.op == OpEntryPointOp) {
            // OpEntryPoint model id name... interface ids: the name is a literal string. The old input leaves the interface and its new
            // inputs join it.
            const char* nm = reinterpret_cast<const char*>(&c[3]);
            size_t chars = 0;
            while (chars < (c.size() - 3) * 4 && nm[chars]) ++chars;
            const size_t iface = 3 + chars / 4 + 1;
            std::vector<uint32_t> nc(c.begin(), c.begin() + static_cast<std::ptrdiff_t>(iface));
            for (size_t k = iface; k < c.size(); ++k) {
                if (old_to_work.count(c[k])) {
                    for (const Elem& el : work[old_to_work[c[k]]].elems) nc.push_back(el.new_var);
                } else {
                    nc.push_back(c[k]);
                }
            }
            nc[0] = (static_cast<uint32_t>(nc.size()) << 16) | (nc[0] & 0xffff);
            c = std::move(nc);
        }
        out.insert(out.end(), c.begin(), c.end());
    }
    out[3] = bound;
    return true;
}


std::vector<SpvIntSlot> spirv_predict_int_inputs(const std::vector<uint32_t>& in) {
    enum : uint32_t { OpTypeIntOp = 21, OpTypeFloatOp = 22, OpTypeVectorOp = 23, OpTypeArrayOp = 28, OpTypePointerOp = 32, OpConstantOp = 43, OpVariableOp = 59,
                      OpLoadOp = 61, OpStoreOp = 62, OpAccessChainOp = 65, OpInBoundsAccessChainOp = 66, OpDecorateOp = 71, OpBitcastOp = 124, StorageInputC = 1 };
    std::vector<SpvIntSlot> result;
    if (in.size() < 5) return result;
    uint32_t f32 = 0, v4 = 0;
    std::map<uint32_t, bool> is_int32;                               // type id -> 32-bit integer
    std::map<uint32_t, uint32_t> ptr_pointee, array_elem, array_len, const_value, location, var_ptr;
    std::map<uint32_t, uint32_t> var_elems;                          // candidate Input variable -> number of vec4 elements (1 for a plain vec4)
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return result;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpTypeFloatOp && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeIntOp && n.len == 4 && w[2] == 32) is_int32[w[1]] = true;
        else if (n.op == OpTypeVectorOp && n.len == 4 && w[3] == 4 && w[2] == f32 && f32) v4 = w[1];
        else if (n.op == OpTypeArrayOp && n.len == 4) { array_elem[w[1]] = w[2]; array_len[w[1]] = w[3]; }
        else if (n.op == OpConstantOp && n.len == 4) const_value[w[2]] = w[3];
        else if (n.op == OpTypePointerOp && n.len == 4 && w[2] == StorageInputC) ptr_pointee[w[1]] = w[3];
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpVariableOp && n.len >= 4 && w[3] == StorageInputC) var_ptr[w[2]] = w[1];
    }
    if (!f32 || !v4) return result;
    for (const auto& [var, ptr] : var_ptr) {
        if (!location.count(var) || !ptr_pointee.count(ptr)) continue;
        const uint32_t t = ptr_pointee[ptr];
        if (t == v4) var_elems[var] = 1;
        else if (array_elem.count(t) && array_elem[t] == v4 && const_value.count(array_len[t]) && const_value[array_len[t]] <= 16) var_elems[var] = const_value[array_len[t]];
    }
    if (var_elems.empty()) return result;
    // use counts per location: [0] bitcast to a 32-bit integer, [1] anything else
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> uses;
    std::map<uint32_t, uint32_t> chain_loc;                          // access chain result -> location
    std::map<uint32_t, uint32_t> load_loc;                           // load result -> location
    std::map<uint32_t, uint32_t> pvar_loc;                           // local variable that holds a loaded attribute -> location
    auto spoil_var = [&](uint32_t var) { for (uint32_t e = 0; e < var_elems[var]; ++e) ++uses[location[var] + e].second; };
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if ((n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) && n.len >= 5 && var_elems.count(w[3])) {
            const uint32_t var = w[3];
            const uint32_t indices = n.len - 4;
            const bool array = var_elems[var] > 1 || indices == 2;
            const uint32_t want = array ? 2 : 1;
            // a computed index reads whichever attribute the shader picks at run time: not counted either way
            if (indices != want || !const_value.count(w[4]) || (want == 2 && !const_value.count(w[5]))) continue;
            const uint32_t elem = array ? const_value[w[4]] : 0;
            if (elem >= var_elems[var]) continue;
            chain_loc[w[2]] = location[var] + elem;
        } else if (n.op == OpLoadOp && n.len >= 4) {
            if (chain_loc.count(w[3])) load_loc[w[2]] = chain_loc[w[3]];
            else if (pvar_loc.count(w[3])) load_loc[w[2]] = pvar_loc[w[3]];          // the value came back out of a local variable
            else if (var_elems.count(w[3])) spoil_var(w[3]);
        } else if (n.op == OpStoreOp && n.len >= 3 && load_loc.count(w[2]) && !chain_loc.count(w[1]) && !var_elems.count(w[1])) {
            auto it = pvar_loc.find(w[1]);                                            // stored into a local variable: later loads of it stand for the attribute
            if (it != pvar_loc.end() && it->second != load_loc[w[2]]) { ++uses[it->second].second; ++uses[load_loc[w[2]]].second; }
            else pvar_loc[w[1]] = load_loc[w[2]];
        }
    }
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpLoadOp || n.op == OpAccessChainOp || n.op == OpInBoundsAccessChainOp) continue;
        if (n.op == OpStoreOp && n.len >= 3 && pvar_loc.count(w[1])) continue;          // the store into a local variable was followed above
        for (uint32_t k = 1; k < n.len; ++k) {
            auto it = load_loc.find(w[k]);
            if (it == load_loc.end()) continue;
            if (n.op == OpBitcastOp && n.len == 4 && k == 3 && is_int32.count(w[1])) ++uses[it->second].first;
            else ++uses[it->second].second;
        }
    }
    for (const auto& [loc, u] : uses)
        if (u.first > 0 && u.second == 0) {
            SpvIntSlot sl;
            sl.location = loc;
            sl.is_signed = false;
            result.push_back(sl);
        }
    return result;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_init_outputs(const std::vector<uint32_t>& in, std::vector<uint32_t>& out) {
    enum : uint32_t { OpLineOp = 8, OpTypeArrayOp = 28, OpConstantOp = 43, OpConstantCompositeOp = 44, OpStoreOp = 62, OpLabelOp = 248,
                      OpNoLineOp = 317, OpEmitVertexOp = 218, OpEmitStreamVertexOp = 220, DecBuiltIn = 11, DecComponent = 31,
                      ModelVertex = 0, ModelTessEval = 2, ModelGeometry = 3 };
    if (in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    size_t entry_at = 0, first_function = 0;
    uint32_t entries = 0;
    struct Scalar { bool is_float; uint32_t width; };
    std::map<uint32_t, Scalar> scalars;                                   // OpTypeFloat / OpTypeInt id -> kind, width
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> vec_of;             // vector type -> (component type, count)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> array_of;           // array type -> (element type, length constant id)
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> ptr_of;             // pointer type -> (storage class, pointee)
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> constants;          // (type, value) -> OpConstant id (one-word ones)
    std::map<uint32_t, uint32_t> const_value, location, component;
    std::map<uint32_t, bool> builtin;
    std::vector<std::pair<uint32_t, uint32_t>> output_vars;               // (variable id, pointer type) in declaration order
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpEntryPoint && n.len >= 4) { if (!entries++) entry_at = n.at; }
        else if (n.op == OpFunction) { if (!first_function) first_function = n.at; }
        else if (n.op == OpTypeFloat && n.len >= 3) scalars[w[1]] = {true, w[2]};
        else if (n.op == OpTypeInt && n.len == 4) scalars[w[1]] = {false, w[2]};
        else if (n.op == OpTypeVector && n.len == 4) vec_of[w[1]] = {w[2], w[3]};
        else if (n.op == OpTypeArrayOp && n.len == 4) array_of[w[1]] = {w[2], w[3]};
        else if (n.op == OpTypePointer && n.len == 4) ptr_of[w[1]] = {w[2], w[3]};
        else if (n.op == OpConstantOp && n.len == 4) { const_value[w[2]] = w[3]; constants.emplace(std::make_pair(w[1], w[3]), w[2]); }
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecorationLocation) location[w[1]] = w[3];
        else if (n.op == OpDecorate && n.len >= 4 && w[2] == DecComponent) component[w[1]] = w[3];
        else if (n.op == OpDecorate && n.len >= 3 && w[2] == DecBuiltIn) builtin[w[1]] = true;
        else if (n.op == OpVariable && n.len == 4 && w[3] == StorageOutput && !first_function) output_vars.emplace_back(w[2], w[1]);   // no initializer
    }
    if (entries != 1 || !first_function) return false;
    const uint32_t* ep = &in[entry_at];
    const uint32_t ep_len = in[entry_at] >> 16, model = ep[1], entry_func = ep[2];
    if (model != ModelVertex && model != ModelTessEval && model != ModelGeometry) return false;
    // OpEntryPoint model id name... interface ids: the name is a literal string. Only a variable the entry point lists is stored to (before
    // SPIR-V 1.4 the list must hold every Output variable the entry point uses).
    std::map<uint32_t, bool> in_interface;
    {
        const char* nm = reinterpret_cast<const char*>(&ep[3]);
        size_t chars = 0;
        while (chars < (ep_len - 3) * 4 && nm[chars]) ++chars;
        for (size_t k = 3 + chars / 4 + 1; k < ep_len; ++k) in_interface[ep[k]] = true;
    }

    // The value each output starts with: component 3 is 1, the others 0 (1.0f for a float, 1 for an integer), counted from the variable's
    // Component decoration. An Output array of such vectors gets it in every element. New constants get ids from the bound.
    std::vector<uint32_t> decls;
    auto scalar_const = [&](uint32_t type, uint32_t value) -> uint32_t {
        auto it = constants.find({type, value});
        if (it != constants.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpConstantOp, type, id, value});
        constants[{type, value}] = id;
        return id;
    };
    std::map<std::vector<uint32_t>, uint32_t> composites;                 // (type, constituents...) -> OpConstantComposite id
    auto composite_const = [&](uint32_t type, const std::vector<uint32_t>& parts) -> uint32_t {
        std::vector<uint32_t> key{type};
        key.insert(key.end(), parts.begin(), parts.end());
        auto it = composites.find(key);
        if (it != composites.end()) return it->second;
        const uint32_t id = bound++;
        decls.push_back(((3u + static_cast<uint32_t>(parts.size())) << 16) | OpConstantCompositeOp);
        decls.push_back(type);
        decls.push_back(id);
        decls.insert(decls.end(), parts.begin(), parts.end());
        composites[key] = id;
        return id;
    };
    // the start value of a scalar or vector of 32-bit float / int whose first component sits in slot `first`; 0 for any other type
    auto start_value = [&](uint32_t type, uint32_t first) -> uint32_t {
        uint32_t comp = type, count = 1;
        const bool vector = vec_of.count(type) != 0;
        if (vector) { comp = vec_of[type].first; count = vec_of[type].second; }
        auto sc = scalars.find(comp);
        if (sc == scalars.end() || sc->second.width != 32 || count < 1 || count > 4) return 0;
        const uint32_t one = sc->second.is_float ? 0x3f800000u : 1u;
        std::vector<uint32_t> parts;
        for (uint32_t c = 0; c < count; ++c) parts.push_back(scalar_const(comp, first + c == 3 ? one : 0u));
        return vector ? composite_const(type, parts) : parts[0];
    };
    std::vector<uint32_t> stores;
    for (const auto& [var, ptr] : output_vars) {
        if (!location.count(var) || builtin.count(var) || !in_interface.count(var)) continue;
        auto p = ptr_of.find(ptr);
        if (p == ptr_of.end() || p->second.first != StorageOutput) continue;
        const uint32_t type = p->second.second, first = component.count(var) ? component[var] : 0u;
        uint32_t value = 0;
        if (array_of.count(type)) {
            const auto [elem, len_id] = array_of[type];
            if (!const_value.count(len_id) || const_value[len_id] == 0 || const_value[len_id] > 64) continue;
            const uint32_t e = start_value(elem, first);
            if (!e) continue;
            value = composite_const(type, std::vector<uint32_t>(const_value[len_id], e));
        } else {
            value = start_value(type, first);
        }
        if (value) stores.insert(stores.end(), {(3u << 16) | OpStoreOp, var, value});
    }
    if (stores.empty()) return false;

    // the stores open the entry function, behind the leading OpVariable run of its first block
    size_t insert_at = 0;
    for (size_t k = 0; k < insts.size() && !insert_at; ++k) {
        if (insts[k].op != OpFunction || in[insts[k].at + 2] != entry_func) continue;
        size_t j = k + 1;
        while (j < insts.size() && insts[j].op != OpLabelOp && insts[j].op != OpFunction) ++j;
        if (j >= insts.size() || insts[j].op != OpLabelOp) return false;
        ++j;
        while (j < insts.size() && (insts[j].op == OpVariable || insts[j].op == OpLineOp || insts[j].op == OpNoLineOp)) ++j;
        if (j < insts.size()) insert_at = insts[j].at;
    }
    if (!insert_at) return false;

    out.clear();
    out.reserve(in.size() + decls.size() + stores.size() * 4 + 8);
    out.insert(out.end(), in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        if (n.at == first_function) out.insert(out.end(), decls.begin(), decls.end());
        if (n.at == insert_at) out.insert(out.end(), stores.begin(), stores.end());
        out.insert(out.end(), in.begin() + static_cast<std::ptrdiff_t>(n.at), in.begin() + static_cast<std::ptrdiff_t>(n.at + n.len));
        // a geometry shader's outputs are undefined again after every emitted vertex (Vulkan). Maxwell keeps them instead, so a shader that
        // wrote an output once and emitted it twice would read (0,0,0,1) the second time; every GS this game ships rewrites each output
        // before each emit, or returns right after its only one. A shader that relies on it needs Private shadow copies flushed at each emit.
        if (model == ModelGeometry && (n.op == OpEmitVertexOp || n.op == OpEmitStreamVertexOp)) out.insert(out.end(), stores.begin(), stores.end());
    }
    out[3] = bound;
    return true;
}

}  // namespace nxrt::nvn::be::gpu

namespace nxrt::nvn::be::gpu {

bool spirv_int_samplers(const std::vector<uint32_t>& in, const std::vector<SpvIntSampler>& slots, std::vector<uint32_t>& out) {
    enum : uint32_t { OpTypeIntOp = 21, OpTypeFloatOp = 22, OpTypeVectorOp = 23, OpTypeImageOp = 25, OpTypeSampledImageOp = 27,
                      OpTypePointerOp = 32, OpFunctionOp = 54, OpVariableOp = 59, OpLoadOp = 61, OpDecorateOp = 71,
                      OpImageSampleImplicitLodOp = 87, OpImageSampleExplicitLodOp = 88, OpImageSampleProjImplicitLodOp = 91,
                      OpImageSampleProjExplicitLodOp = 92, OpImageFetchOp = 95, OpImageGatherOp = 96, OpImageOp = 100,
                      OpImageQuerySizeLodOp = 103, OpImageQuerySizeOp = 104, OpImageQueryLodOp = 105, OpImageQueryLevelsOp = 106,
                      OpImageQuerySamplesOp = 107, OpBitcastOp = 124, DecBinding = 33, DecDescriptorSet = 34, StorageUniformConstant = 0 };
    if (slots.empty() || in.size() < 5) return false;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return false;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t bound = in[3];
    uint32_t f32 = 0, int_type[2] = {0, 0};                        // [0] uint, [1] int (32-bit)
    std::map<uint32_t, uint32_t> set_of, binding_of, sampled_of, ptr_pointee, var_ptr;
    std::map<uint32_t, std::vector<uint32_t>> image_words;         // OpTypeImage id -> its operands after the result id
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> vec_of;      // (component type, count) -> vector type
    std::map<uint32_t, std::pair<uint32_t, uint32_t>> vec_parts;   // vector type -> (component type, count)
    std::map<std::pair<uint32_t, uint32_t>, uint32_t> ptr_of;      // (storage, pointee) -> pointer type
    size_t first_function = 0;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpFunctionOp) { if (!first_function) first_function = n.at; continue; }
        if (first_function) continue;
        if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecDescriptorSet) set_of[w[1]] = w[3];
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecBinding) binding_of[w[1]] = w[3];
        else if (n.op == OpTypeFloatOp && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeIntOp && n.len == 4 && w[2] == 32) int_type[w[3] ? 1 : 0] = w[1];
        else if (n.op == OpTypeVectorOp && n.len == 4) { vec_of[{w[2], w[3]}] = w[1]; vec_parts[w[1]] = {w[2], w[3]}; }
        else if (n.op == OpTypeImageOp && n.len >= 9) image_words[w[1]] = std::vector<uint32_t>(w + 2, w + n.len);
        else if (n.op == OpTypeSampledImageOp && n.len == 3) sampled_of[w[1]] = w[2];
        else if (n.op == OpTypePointerOp && n.len == 4) { ptr_pointee[w[1]] = w[3]; ptr_of.emplace(std::make_pair(w[2], w[3]), w[1]); }
        else if (n.op == OpVariableOp && n.len >= 4 && w[3] == StorageUniformConstant) var_ptr[w[2]] = w[1];
    }
    if (!f32 || !first_function) return false;

    // New declarations go right before the first function: every existing type is declared by then, and the variables that change move
    // there behind their new types. SPIR-V allows no second OpTypeInt / OpTypeVector / OpTypeImage / OpTypeSampledImage with the same
    // operands, so the existing ones are taken when they exist.
    std::vector<uint32_t> decls;
    auto scalar_for = [&](int sign) -> uint32_t {
        if (!int_type[sign]) {
            int_type[sign] = bound++;
            decls.insert(decls.end(), {(4u << 16) | OpTypeIntOp, int_type[sign], 32u, static_cast<uint32_t>(sign)});
        }
        return int_type[sign];
    };
    auto vec_for = [&](uint32_t comp, uint32_t count) -> uint32_t {
        auto it = vec_of.find({comp, count});
        if (it != vec_of.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypeVectorOp, id, comp, count});
        vec_of[{comp, count}] = id;
        vec_parts[id] = {comp, count};
        return id;
    };
    auto image_for = [&](const std::vector<uint32_t>& ops) -> uint32_t {
        for (const auto& [id, o] : image_words) if (o == ops) return id;
        const uint32_t id = bound++;
        decls.push_back(((static_cast<uint32_t>(ops.size()) + 2u) << 16) | OpTypeImageOp);
        decls.push_back(id);
        decls.insert(decls.end(), ops.begin(), ops.end());
        image_words[id] = ops;
        return id;
    };
    auto sampled_for = [&](uint32_t image) -> uint32_t {
        for (const auto& [id, img] : sampled_of) if (img == image) return id;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(3u << 16) | OpTypeSampledImageOp, id, image});
        sampled_of[id] = image;
        return id;
    };
    auto ptr_for = [&](uint32_t pointee) -> uint32_t {
        auto it = ptr_of.find({StorageUniformConstant, pointee});
        if (it != ptr_of.end()) return it->second;
        const uint32_t id = bound++;
        decls.insert(decls.end(), {(4u << 16) | OpTypePointerOp, id, StorageUniformConstant, pointee});
        ptr_of[{StorageUniformConstant, pointee}] = id;
        return id;
    };

    struct Work { uint32_t new_si, new_img; int sign; };
    std::map<uint32_t, Work> work;                                 // by variable
    for (const SpvIntSampler& sl : slots) {
        uint32_t var = 0;
        for (const auto& [v, p] : var_ptr)
            if (set_of.count(v) && binding_of.count(v) && set_of[v] == sl.set && binding_of[v] == sl.binding) var = v;
        if (!var || work.count(var) || !ptr_pointee.count(var_ptr[var])) return false;
        const uint32_t si = ptr_pointee[var_ptr[var]];
        if (!sampled_of.count(si)) return false;                   // an array of samplers, a storage image, ...: not handled
        auto iw = image_words.find(sampled_of[si]);
        // OpTypeImage operands: sampled type, Dim, Depth, Arrayed, MS, Sampled, format. A float, non-depth, sampled image only.
        if (iw == image_words.end() || iw->second.size() < 7 || iw->second[0] != f32 || iw->second[2] == 1 || iw->second[5] != 1) return false;
        const int sign = sl.is_signed ? 1 : 0;
        std::vector<uint32_t> ops = iw->second;
        ops[0] = scalar_for(sign);
        const uint32_t new_img = image_for(ops);
        work[var] = {sampled_for(new_img), new_img, sign};
    }
    std::vector<uint32_t> var_decls;
    for (const auto& [var, wk] : work) var_decls.insert(var_decls.end(), {(4u << 16) | OpVariableOp, ptr_for(wk.new_si), var, StorageUniformConstant});

    // The function bodies: loads of the variables get the new sampled image type, OpImage of those the new image type; each sample,
    // gather and fetch through them returns the integer vector, and an OpBitcast hands the shader the same bits in the float vector it
    // declared (the console's registers hold the integer texel bits). Any other use of these ids: not handled, the module is left alone.
    std::map<uint32_t, const Work*> vars, loaded, images;          // variable / loaded sampled image / OpImage result -> its work
    for (const auto& [var, wk] : work) vars[var] = &wk;
    struct Edit { uint32_t new_type = 0, new_result = 0, cast_type = 0; };
    std::map<size_t, Edit> edits;                                  // by instruction position
    auto mentions = [&](const uint32_t* w, uint32_t len, const std::map<uint32_t, const Work*>& ids, uint32_t from) {
        for (uint32_t k = from; k < len; ++k) if (ids.count(w[k])) return true;
        return false;
    };
    for (const Inst& n : insts) {
        if (n.at < first_function) continue;
        const uint32_t* w = &in[n.at];
        if (n.op == OpLoadOp && n.len >= 4 && vars.count(w[3])) {
            loaded[w[2]] = vars[w[3]];
            edits[n.at].new_type = vars[w[3]]->new_si;
            continue;
        }
        if (n.op == OpImageOp && n.len == 4 && loaded.count(w[3])) {
            images[w[2]] = loaded[w[3]];
            edits[n.at].new_type = loaded[w[3]]->new_img;
            continue;
        }
        const bool sample = n.op == OpImageSampleImplicitLodOp || n.op == OpImageSampleExplicitLodOp || n.op == OpImageSampleProjImplicitLodOp ||
                            n.op == OpImageSampleProjExplicitLodOp || n.op == OpImageGatherOp;
        const Work* wk = nullptr;
        if (sample && n.len >= 5 && loaded.count(w[3])) wk = loaded[w[3]];
        else if (n.op == OpImageFetchOp && n.len >= 5 && images.count(w[3])) wk = images[w[3]];
        if (wk) {
            auto vp = vec_parts.find(w[1]);
            if (vp == vec_parts.end() || vp->second.first != f32) return false;
            if (mentions(w, n.len, loaded, 4) || mentions(w, n.len, images, 4) || mentions(w, n.len, vars, 1)) return false;
            Edit& e = edits[n.at];
            e.new_type = vec_for(int_type[wk->sign], vp->second.second);
            e.new_result = bound++;
            e.cast_type = w[1];
            continue;
        }
        const bool query = n.op == OpImageQuerySizeLodOp || n.op == OpImageQuerySizeOp || n.op == OpImageQueryLodOp || n.op == OpImageQueryLevelsOp ||
                           n.op == OpImageQuerySamplesOp;
        if (query && n.len >= 4 && (loaded.count(w[3]) || images.count(w[3])) && !mentions(w, n.len, loaded, 4) && !mentions(w, n.len, images, 4)) continue;
        // a use the rewrite does not know (OpCopyObject, OpPhi, a function argument, a depth-compare sample, ...)
        if (mentions(w, n.len, vars, 1) || mentions(w, n.len, loaded, 1) || mentions(w, n.len, images, 1)) return false;
    }
    if (edits.empty()) return false;

    out.clear();
    out.reserve(in.size() + decls.size() + var_decls.size() + 4 * edits.size());
    out.insert(out.end(), in.begin(), in.begin() + 5);
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.at == first_function) {
            out.insert(out.end(), decls.begin(), decls.end());
            out.insert(out.end(), var_decls.begin(), var_decls.end());
        }
        if (n.at < first_function && n.op == OpVariableOp && n.len >= 4 && work.count(w[2])) continue;   // declared again above
        const size_t start = out.size();
        out.insert(out.end(), w, w + n.len);
        auto e = edits.find(n.at);
        if (e == edits.end()) continue;
        out[start + 1] = e->second.new_type;
        if (e->second.new_result) {
            out[start + 2] = e->second.new_result;
            out.insert(out.end(), {(4u << 16) | OpBitcastOp, e->second.cast_type, w[2], e->second.new_result});
        }
    }
    out[3] = bound;
    return true;
}

std::vector<SpvIntSampler> spirv_predict_int_fetches(const std::vector<uint32_t>& in) {
    enum : uint32_t { OpTypeIntOp = 21, OpTypeFloatOp = 22, OpTypeImageOp = 25, OpTypeSampledImageOp = 27, OpTypePointerOp = 32,
                      OpFunctionOp = 54, OpVariableOp = 59, OpLoadOp = 61, OpDecorateOp = 71, OpCompositeExtractOp = 81,
                      OpImageSampleImplicitLodOp = 87, OpImageSparseFirstOp = 305, OpImageFetchOp = 95, OpImageOp = 100, OpBitcastOp = 124,
                      DecBinding = 33, DecDescriptorSet = 34, StorageUniformConstant = 0, Dim3D = 2 };
    std::vector<SpvIntSampler> slots;
    if (in.size() < 5 || in[0] != 0x07230203u) return slots;
    std::vector<Inst> insts;
    for (size_t i = 5; i < in.size();) {
        const uint32_t len = in[i] >> 16;
        if (!len || i + len > in.size()) return slots;
        insts.push_back({i, in[i] & 0xffff, len});
        i += len;
    }
    uint32_t f32 = 0;
    std::map<uint32_t, bool> int32;                                // 32-bit integer type ids
    std::map<uint32_t, uint32_t> set_of, binding_of, sampled_of, ptr_pointee;
    std::map<uint32_t, std::vector<uint32_t>> image_words;
    std::vector<uint32_t> vars;                                    // candidate variables, in declaration order
    std::map<uint32_t, bool> cand;
    bool in_functions = false;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpFunctionOp) { in_functions = true; break; }
        if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecDescriptorSet) set_of[w[1]] = w[3];
        else if (n.op == OpDecorateOp && n.len >= 4 && w[2] == DecBinding) binding_of[w[1]] = w[3];
        else if (n.op == OpTypeFloatOp && n.len >= 3 && w[2] == 32) f32 = w[1];
        else if (n.op == OpTypeIntOp && n.len == 4 && w[2] == 32) int32[w[1]] = true;
        else if (n.op == OpTypeImageOp && n.len >= 9) image_words[w[1]] = std::vector<uint32_t>(w + 2, w + n.len);
        else if (n.op == OpTypeSampledImageOp && n.len == 3) sampled_of[w[1]] = w[2];
        else if (n.op == OpTypePointerOp && n.len == 4 && w[2] == StorageUniformConstant) ptr_pointee[w[1]] = w[3];
        else if (n.op == OpVariableOp && n.len >= 4 && w[3] == StorageUniformConstant) {
            auto p = ptr_pointee.find(w[1]);
            if (p == ptr_pointee.end()) continue;
            auto s = sampled_of.find(p->second);
            if (s == sampled_of.end()) continue;               // an array of samplers, a storage image, a separate image: not handled
            auto iw = image_words.find(s->second);
            // OpTypeImage operands: sampled type, Dim, Depth, Arrayed, MS, Sampled, format. A float, non-depth, single-sample sampled image.
            // 3D only: the light grid is a 3D image, and a 2D fetch bitcast to integers is as often a bitwise copy of a float or UNORM
            // texture (which a UINT view would turn into the dummy).
            if (iw == image_words.end() || iw->second.size() < 7 || iw->second[0] != f32 || iw->second[1] != Dim3D || iw->second[2] == 1 ||
                iw->second[4] != 0 || iw->second[5] != 1) continue;
            if (!set_of.count(w[2]) || !binding_of.count(w[2])) continue;
            cand[w[2]] = true;
            vars.push_back(w[2]);
        }
    }
    if (!f32 || cand.empty() || !in_functions) return slots;

    // Loads of the candidates, OpImage of those loads, fetches through those images. A sample, gather or depth compare through a candidate
    // rules it out (a float texture); any other use is left to spirv_int_samplers, which then declines the module as a whole.
    std::map<uint32_t, uint32_t> loaded, images, fetched, extracted;   // id -> its variable
    std::map<uint32_t, size_t> def_at;                                 // fetch / extract result -> its defining instruction
    std::map<uint32_t, bool> bad, has_fetch;
    for (const Inst& n : insts) {
        const uint32_t* w = &in[n.at];
        if (n.op == OpLoadOp && n.len >= 4 && cand.count(w[3])) loaded[w[2]] = w[3];
        else if (n.op == OpImageOp && n.len == 4 && loaded.count(w[3])) images[w[2]] = loaded[w[3]];
        else if (n.op == OpImageFetchOp && n.len >= 5 && images.count(w[3])) {
            fetched[w[2]] = images[w[3]];
            def_at[w[2]] = n.at;
            has_fetch[images[w[3]]] = true;
        } else if (((n.op >= OpImageSampleImplicitLodOp && n.op <= 97) || (n.op >= OpImageSparseFirstOp && n.op <= 315)) && n.len >= 4 && loaded.count(w[3])) {
            bad[loaded[w[3]]] = true;
        }
    }
    if (fetched.empty()) return slots;
    // Every use of a fetch result is an OpCompositeExtract of it (pass 0), every use of such an extract an OpBitcast to a 32-bit integer
    // type (pass 1). The defining instruction is not a use.
    for (int pass = 0; pass < 2; ++pass) {
        const std::map<uint32_t, uint32_t> ids = pass == 0 ? fetched : extracted;
        for (const Inst& n : insts) {
            const uint32_t* w = &in[n.at];
            for (uint32_t k = 1; k < n.len; ++k) {
                auto it = ids.find(w[k]);
                if (it == ids.end()) continue;
                auto d = def_at.find(w[k]);
                if (k == 2 && d != def_at.end() && d->second == n.at) continue;
                const bool ok = pass == 0 ? (n.op == OpCompositeExtractOp && k == 3 && n.len >= 5)
                                          : (n.op == OpBitcastOp && k == 3 && n.len == 4 && int32.count(w[1]));
                if (!ok) { bad[it->second] = true; continue; }
                if (pass == 0) {
                    extracted[w[2]] = it->second;
                    def_at[w[2]] = n.at;
                }
            }
        }
    }
    for (uint32_t v : vars)
        if (has_fetch.count(v) && !bad.count(v)) slots.push_back({set_of[v], binding_of[v], false});
    return slots;
}

}  // namespace nxrt::nvn::be::gpu
