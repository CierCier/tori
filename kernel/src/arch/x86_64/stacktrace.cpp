#include <tori/kernel/stacktrace.hpp>
#include <tori/kernel/demangle.hpp>
#include <stdint.h>
#include <stddef.h>

// ---------------------------------------------------------------------------
// DWARF / .eh_frame constants                                              |
// ---------------------------------------------------------------------------
enum : uint8_t {
    DW_CFA_advance_loc  = 0x40,
    DW_CFA_offset       = 0x80,
    DW_CFA_restore      = 0xC0,

    DW_CFA_nop          = 0x00,
    DW_CFA_set_loc      = 0x01,
    DW_CFA_advance_loc1 = 0x02,
    DW_CFA_advance_loc2 = 0x03,
    DW_CFA_advance_loc4 = 0x04,
    DW_CFA_offset_extended       = 0x05,
    DW_CFA_restore_extended      = 0x06,
    DW_CFA_undefine              = 0x07,
    DW_CFA_same_value            = 0x08,
    DW_CFA_register              = 0x09,
    DW_CFA_remember_state        = 0x0A,
    DW_CFA_restore_state         = 0x0B,
    DW_CFA_def_cfa               = 0x0C,
    DW_CFA_def_cfa_register      = 0x0D,
    DW_CFA_def_cfa_offset        = 0x0E,
    DW_CFA_def_cfa_expression    = 0x0F,
    DW_CFA_expression            = 0x10,
    DW_CFA_offset_extended_sf    = 0x11,
    DW_CFA_def_cfa_sf            = 0x12,
    DW_CFA_def_cfa_offset_sf     = 0x13,
    DW_CFA_val_offset            = 0x14,
    DW_CFA_val_offset_sf         = 0x15,
    DW_CFA_val_expression        = 0x16,
    DW_CFA_GNU_args_size         = 0x2E,
    DW_CFA_GNU_negative_offset_extended = 0x2F,
};

enum : uint8_t {
    DW_EH_PE_absptr   = 0x00,
    DW_EH_PE_uleb128  = 0x01,
    DW_EH_PE_udata2   = 0x02,
    DW_EH_PE_udata4   = 0x03,
    DW_EH_PE_udata8   = 0x04,
    DW_EH_PE_sleb128  = 0x09,
    DW_EH_PE_sdata2   = 0x0A,
    DW_EH_PE_sdata4   = 0x0B,
    DW_EH_PE_sdata8   = 0x0C,
    DW_EH_PE_pcrel    = 0x10,
    DW_EH_PE_datarel  = 0x30,
    DW_EH_PE_omit     = 0xFF,
};

enum : uint8_t { DW_REG_RBP = 6, DW_REG_RSP = 7, DW_REG_RA = 16 };

// ---------------------------------------------------------------------------
// Linker symbols
// ---------------------------------------------------------------------------
extern "C" char __eh_frame_start[];
extern "C" char __eh_frame_end[];
extern "C" char __eh_frame_hdr_start[];
extern "C" char __eh_frame_hdr_end[];

struct SymEntry { uint64_t addr; const char* name; };
extern "C" SymEntry __kernel_symtab_start[] __attribute__((weak));
extern "C" size_t __kernel_symtab_count __attribute__((weak));

// ---------------------------------------------------------------------------
// ULEB128 / SLEB128 / encoded-pointer helpers
// ---------------------------------------------------------------------------
static uint64_t read_uleb(const uint8_t*& p) {
    uint64_t v = 0; int s = 0; uint8_t b;
    do { b = *p++; v |= (uint64_t)(b & 0x7F) << s; s += 7; } while (b & 0x80);
    return v;
}
static int64_t read_sleb(const uint8_t*& p) {
    int64_t v = 0; int s = 0; uint8_t b;
    do { b = *p++; v |= (int64_t)(b & 0x7F) << s; s += 7; } while (b & 0x80);
    if (s < 64 && (b & 0x40)) v |= -(1LL << s);
    return v;
}
static uint64_t read_enc(const uint8_t*& p, uint8_t enc, uint64_t db) {
    if (enc == DW_EH_PE_omit) return 0;
    const uint8_t* start = p; uint64_t v = 0;
    switch (enc & 0x0F) {
    case DW_EH_PE_absptr: v = *(const uint64_t*)p; p += 8; break;
    case DW_EH_PE_uleb128: v = read_uleb(p); break;
    case DW_EH_PE_udata2: v = *(const uint16_t*)p; p += 2; break;
    case DW_EH_PE_udata4: v = *(const uint32_t*)p; p += 4; break;
    case DW_EH_PE_udata8: v = *(const uint64_t*)p; p += 8; break;
    case DW_EH_PE_sleb128: v = (uint64_t)read_sleb(p); break;
    case DW_EH_PE_sdata2: v = (uint64_t)(int64_t)*(const int16_t*)p; p += 2; break;
    case DW_EH_PE_sdata4: v = (uint64_t)(int64_t)*(const int32_t*)p; p += 4; break;
    case DW_EH_PE_sdata8: v = *(const uint64_t*)p; p += 8; break;
    default: return 0;
    }
    if ((enc & 0xF0) == DW_EH_PE_pcrel) v += (uint64_t)start;
    else if ((enc & 0xF0) == DW_EH_PE_datarel) v += db;
    return v;
}

// ---------------------------------------------------------------------------
// CIE parsing
// ---------------------------------------------------------------------------
struct Cie {
    uint64_t code_align;
    int64_t data_align;
    uint8_t ra_reg;
    uint8_t fde_enc;
    const uint8_t* inst;
    const uint8_t* inst_end;
    uint64_t datarel_base;
    bool has_z;
};

static bool parse_cie(const uint8_t* body, uint64_t len, bool is64, Cie& c) {
    const uint8_t* end = body + len;
    const uint8_t* p = body;
    if (is64) { if (*(const uint64_t*)p) return false; p += 8; }
    else      { if (*(const uint32_t*)p) return false; p += 4; }
    uint8_t ver = *p++;
    if (ver != 1 && ver != 3 && ver != 4) return false;
    const char* aug = (const char*)p; while (*p++) {}
    c.has_z = false; c.fde_enc = DW_EH_PE_absptr;
    bool has_L = false, has_P = false;
    for (const char* a = aug; *a; ++a) {
        if (*a == 'z') c.has_z = true;
        else if (*a == 'R') /* R flag */;
        else if (*a == 'L') has_L = true;
        else if (*a == 'P') has_P = true;
        else return false;
    }
    if (ver >= 4) p += 2; // address_size + segment_size
    c.code_align = read_uleb(p);
    c.data_align = read_sleb(p);
    c.ra_reg = read_uleb(p);
    c.datarel_base = (uint64_t)body;
    if (c.has_z) {
        uint64_t alen = read_uleb(p);
        const uint8_t* aend = p + alen;
        if (has_L) { uint8_t le = *p++; (void)le; }
        if (has_P) { uint8_t pe = *p++; read_enc(p, pe, c.datarel_base); }
        uint8_t re = 0;
        for (const char* a = aug; *a; ++a) {
            if (*a == 'R') { re = *p++; break; }
        }
        if (re) c.fde_enc = re;
        p = aend;
    }
    c.inst = p; c.inst_end = end;
    return true;
}

// ---------------------------------------------------------------------------
// FDE parsing
// ---------------------------------------------------------------------------
struct Fde {
    uint64_t start_pc, end_pc;
    uint64_t code_align;
    int64_t data_align;
    uint8_t ra_reg;
    uint8_t fde_enc;
    const uint8_t* cie_inst;
    const uint8_t* cie_inst_end;
    uint64_t cie_datarel_base;
    const uint8_t* fde_inst;
    const uint8_t* fde_inst_end;
};

static const uint8_t* entry_body(const uint8_t* p, uint64_t& len, bool& is64) {
    uint32_t h = *(const uint32_t*)p;
    is64 = (h == 0xFFFFFFFF);
    if (is64) { len = *(const uint64_t*)(p+4); return p+12; }
    if (h == 0) { len = 0; return nullptr; }
    len = h; return p+4;
}

static bool parse_fde(const uint8_t* p, const uint8_t* body, uint64_t len, bool is64, Fde& f) {
    const uint8_t* end = body + len;
    const uint8_t* q = body;
    uint64_t cie_off;
    if (is64) { cie_off = *(const uint64_t*)q; q += 8; }
    else      { cie_off = *(const uint32_t*)q; q += 4; }
    const uint8_t* cie_start = q - (is64?8:4) - cie_off;
    uint64_t cie_len; bool cie_is64;
    const uint8_t* cie_body = entry_body(cie_start, cie_len, cie_is64);
    if (!cie_body) return false;
    Cie c;
    if (!parse_cie(cie_body, cie_len, cie_is64, c)) return false;
    f.code_align = c.code_align; f.data_align = c.data_align; f.ra_reg = c.ra_reg;
    f.fde_enc = c.fde_enc;
    f.cie_inst = c.inst; f.cie_inst_end = c.inst_end; f.cie_datarel_base = c.datarel_base;
    uint64_t db = (uint64_t)p;
    f.start_pc = read_enc(q, c.fde_enc, db);
    f.end_pc = f.start_pc + read_enc(q, c.fde_enc & 0x0F, db);
    if (c.has_z) { uint64_t al = read_uleb(q); q += al; }
    f.fde_inst = q; f.fde_inst_end = end;
    return true;
}

// ---------------------------------------------------------------------------
// CFI state machine
// ---------------------------------------------------------------------------
enum RuleK : uint8_t { RU_UNDEF, RU_SAME, RU_OFFSET, RU_VALOFF, RU_REG };
struct RRule { RuleK k; uint8_t reg; int64_t off; };
struct State {
    uint64_t loc;
    uint8_t cfa_reg; int64_t cfa_off;
    RRule ra, rbp;
};
static const int STK_MAX = 8;

static void exec_cfi(State& st, const uint8_t* p, const uint8_t* end,
                     uint64_t ca, int64_t da, uint8_t fde_enc,
                     uint64_t dbase, uint64_t start, uint64_t target)
{
    st.loc = start;
    State saved[STK_MAX]; int sp = 0;
    while (p < end) {
        uint8_t op = *p++;
        if (op == DW_CFA_nop) continue;
        if ((op & 0xC0) == DW_CFA_advance_loc) {
            st.loc += (op & 0x3F) * ca;
            if (target < st.loc) return;
        } else if ((op & 0xC0) == DW_CFA_offset) {
            uint8_t r = op & 0x3F;
            int64_t o = (int64_t)read_uleb(p) * da;
            if (r == DW_REG_RA) { st.ra.k = RU_OFFSET; st.ra.off = o; }
            else if (r == DW_REG_RBP) { st.rbp.k = RU_OFFSET; st.rbp.off = o; }
        } else if ((op & 0xC0) == DW_CFA_restore) {
            uint8_t r = op & 0x3F;
            if (r == DW_REG_RA) st.ra.k = RU_UNDEF;
            else if (r == DW_REG_RBP) st.rbp.k = RU_UNDEF;
        } else switch (op) {
        case DW_CFA_set_loc:
            st.loc = read_enc(p, fde_enc, dbase);
            if (target < st.loc) return;
            break;
        case DW_CFA_advance_loc1: { st.loc += *p++ * ca; if (target < st.loc) return; break; }
        case DW_CFA_advance_loc2: { st.loc += *(const uint16_t*)p * ca; p+=2; if (target < st.loc) return; break; }
        case DW_CFA_advance_loc4: { st.loc += *(const uint32_t*)p * ca; p+=4; if (target < st.loc) return; break; }
        case DW_CFA_offset_extended: {
            uint8_t r = read_uleb(p); int64_t o = (int64_t)read_uleb(p) * da;
            if (r == DW_REG_RA) { st.ra.k = RU_OFFSET; st.ra.off = o; }
            else if (r == DW_REG_RBP) { st.rbp.k = RU_OFFSET; st.rbp.off = o; }
            break;
        }
        case DW_CFA_restore_extended:
        case DW_CFA_undefine: {
            uint8_t r = read_uleb(p);
            if (r == DW_REG_RA) st.ra.k = RU_UNDEF;
            else if (r == DW_REG_RBP) st.rbp.k = RU_UNDEF;
            break;
        }
        case DW_CFA_same_value: {
            uint8_t r = read_uleb(p);
            if (r == DW_REG_RA) st.ra.k = RU_SAME;
            else if (r == DW_REG_RBP) st.rbp.k = RU_SAME;
            break;
        }
        case DW_CFA_register: {
            uint8_t r = read_uleb(p), vr = read_uleb(p);
            if (r == DW_REG_RA) { st.ra.k = RU_REG; st.ra.reg = vr; }
            else if (r == DW_REG_RBP) { st.rbp.k = RU_REG; st.rbp.reg = vr; }
            break;
        }
        case DW_CFA_remember_state:
            if (sp < STK_MAX) saved[sp++] = st;
            break;
        case DW_CFA_restore_state:
            if (sp > 0) { st = saved[--sp]; }
            break;
        case DW_CFA_def_cfa:
            st.cfa_reg = read_uleb(p); st.cfa_off = (int64_t)read_uleb(p);
            break;
        case DW_CFA_def_cfa_register:
            st.cfa_reg = read_uleb(p);
            break;
        case DW_CFA_def_cfa_offset:
            st.cfa_off = (int64_t)read_uleb(p);
            break;
        case DW_CFA_def_cfa_sf:
            st.cfa_reg = read_uleb(p); st.cfa_off = read_sleb(p) * da;
            break;
        case DW_CFA_def_cfa_offset_sf:
            st.cfa_off = read_sleb(p) * da;
            break;
        case DW_CFA_offset_extended_sf: {
            uint8_t r = read_uleb(p); int64_t o = read_sleb(p) * da;
            if (r == DW_REG_RA) { st.ra.k = RU_OFFSET; st.ra.off = o; }
            else if (r == DW_REG_RBP) { st.rbp.k = RU_OFFSET; st.rbp.off = o; }
            break;
        }
        case DW_CFA_val_offset: {
            uint8_t r = read_uleb(p); int64_t o = (int64_t)read_uleb(p) * da;
            if (r == DW_REG_RBP) { st.rbp.k = RU_VALOFF; st.rbp.off = o; }
            break;
        }
        case DW_CFA_val_offset_sf: {
            uint8_t r = read_uleb(p); int64_t o = read_sleb(p) * da;
            if (r == DW_REG_RBP) { st.rbp.k = RU_VALOFF; st.rbp.off = o; }
            break;
        }
        case DW_CFA_def_cfa_expression:
        case DW_CFA_expression:
        case DW_CFA_val_expression: {
            uint64_t el = read_uleb(p); p += el; break;
        }
        case DW_CFA_GNU_args_size: read_uleb(p); break;
        case DW_CFA_GNU_negative_offset_extended: {
            uint8_t r = read_uleb(p); int64_t o = -(int64_t)read_uleb(p) * da;
            if (r == DW_REG_RA) { st.ra.k = RU_OFFSET; st.ra.off = o; }
            else if (r == DW_REG_RBP) { st.rbp.k = RU_OFFSET; st.rbp.off = o; }
            break;
        }
        default: return;
        }
    }
}

// ---------------------------------------------------------------------------
// FDE lookup
// ---------------------------------------------------------------------------
static const uint8_t* find_fde_hdr(uint64_t pc) {
    const uint8_t* h = (const uint8_t*)__eh_frame_hdr_start;
    const uint8_t* he = (const uint8_t*)__eh_frame_hdr_end;
    if (h >= he) return nullptr;
    const uint8_t* p = h;
    if (*p++ != 1) return nullptr;
    uint8_t eptr_enc = *p++, fcnt_enc = *p++, tbl_enc = *p++;
    uint64_t dbase = (uint64_t)h;
    read_enc(p, eptr_enc, dbase);
    uint64_t cnt = read_enc(p, fcnt_enc, dbase);
    if (cnt == 0 || cnt > 100000) return nullptr;
    // Determine entry stride
    uint8_t fmt = tbl_enc & 0x0F;
    int stride;
    switch (fmt) {
    case DW_EH_PE_absptr: stride = 16; break;
    case DW_EH_PE_udata4:
    case DW_EH_PE_sdata4: stride = 8; break;
    case DW_EH_PE_udata8:
    case DW_EH_PE_sdata8: stride = 16; break;
    case DW_EH_PE_udata2:
    case DW_EH_PE_sdata2: stride = 4; break;
    default: return nullptr;
    }
    int64_t lo = 0, hi = (int64_t)(cnt - 1);
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        const uint8_t* e = p + mid * stride;
        uint64_t epc;
        const uint8_t* tp = e;
        switch (fmt) {
        case DW_EH_PE_absptr: epc = *(const uint64_t*)tp; break;
        case DW_EH_PE_udata4: epc = *(const uint32_t*)tp; break;
        case DW_EH_PE_sdata4: epc = (uint64_t)(int64_t)*(const int32_t*)tp; break;
        case DW_EH_PE_udata8: epc = *(const uint64_t*)tp; break;
        case DW_EH_PE_sdata8: epc = *(const uint64_t*)tp; break;
        case DW_EH_PE_udata2: epc = *(const uint16_t*)tp; break;
        case DW_EH_PE_sdata2: epc = (uint64_t)(int64_t)*(const int16_t*)tp; break;
        default: return nullptr;
        }
        if ((tbl_enc & 0xF0) == DW_EH_PE_datarel)
            epc += dbase;
        if (pc < epc) { hi = mid - 1; continue; }
        // Read FDE address (2nd field)
        tp = e + stride / 2;
        uint64_t fde_addr;
        switch (fmt) {
        case DW_EH_PE_absptr: fde_addr = *(const uint64_t*)tp; break;
        case DW_EH_PE_udata4: fde_addr = *(const uint32_t*)tp; break;
        case DW_EH_PE_sdata4: fde_addr = (uint64_t)(int64_t)*(const int32_t*)tp; break;
        case DW_EH_PE_udata8: fde_addr = *(const uint64_t*)tp; break;
        case DW_EH_PE_sdata8: fde_addr = *(const uint64_t*)tp; break;
        case DW_EH_PE_udata2: fde_addr = *(const uint16_t*)tp; break;
        case DW_EH_PE_sdata2: fde_addr = (uint64_t)(int64_t)*(const int16_t*)tp; break;
        default: return nullptr;
        }
        if ((tbl_enc & 0xF0) == DW_EH_PE_datarel)
            fde_addr += dbase;
        // Verify pc < next entry's pc (or this is the last entry)
        if (mid < (int64_t)(cnt - 1)) {
            const uint8_t* ne = e + stride;
            uint64_t npc;
            tp = ne;
            switch (fmt) {
            case DW_EH_PE_absptr: npc = *(const uint64_t*)tp; break;
            case DW_EH_PE_udata4: npc = *(const uint32_t*)tp; break;
            case DW_EH_PE_sdata4: npc = (uint64_t)(int64_t)*(const int32_t*)tp; break;
            case DW_EH_PE_udata8: npc = *(const uint64_t*)tp; break;
            case DW_EH_PE_sdata8: npc = *(const uint64_t*)tp; break;
            case DW_EH_PE_udata2: npc = *(const uint16_t*)tp; break;
            case DW_EH_PE_sdata2: npc = (uint64_t)(int64_t)*(const int16_t*)tp; break;
            default: return nullptr;
            }
            if ((tbl_enc & 0xF0) == DW_EH_PE_datarel) npc += dbase;
            if (pc >= npc) { lo = mid + 1; continue; }
        }
        return (const uint8_t*)fde_addr;
    }
    return nullptr;
}

static const uint8_t* find_fde_linear(uint64_t pc) {
    const uint8_t* p = (const uint8_t*)__eh_frame_start;
    const uint8_t* end = (const uint8_t*)__eh_frame_end;
    while (p < end) {
        uint64_t len; bool is64;
        const uint8_t* body = entry_body(p, len, is64);
        if (!body || len == 0) break;
        const uint8_t* entry_end = body + len;
        bool is_cie = is64 ? (*(const uint64_t*)body == 0)
                           : (*(const uint32_t*)body == 0);
        if (!is_cie) {
            Fde f;
            if (parse_fde(p, body, len, is64, f) && pc >= f.start_pc && pc < f.end_pc)
                return p;
        }
        p = entry_end;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Unwind step
// ---------------------------------------------------------------------------
static bool step(uint64_t& rip, uint64_t& rsp, uint64_t& rbp) {
    const uint8_t* fe = find_fde_hdr(rip);
    if (!fe) fe = find_fde_linear(rip);
    if (!fe) return false;
    uint64_t len; bool is64;
    const uint8_t* body = entry_body(fe, len, is64);
    if (!body) return false;
    Fde f;
    if (!parse_fde(fe, body, len, is64, f)) return false;
    if (rip < f.start_pc || rip >= f.end_pc) return false;

    State st;
    st.cfa_reg = DW_REG_RSP; st.cfa_off = 0;
    st.ra.k = RU_UNDEF; st.ra.off = 0;
    st.rbp.k = RU_UNDEF; st.rbp.off = 0;

    exec_cfi(st, f.cie_inst, f.cie_inst_end,
             f.code_align, f.data_align, f.fde_enc,
             f.cie_datarel_base, f.start_pc, rip);
    exec_cfi(st, f.fde_inst, f.fde_inst_end,
             f.code_align, f.data_align, f.fde_enc,
             (uint64_t)fe, f.start_pc, rip);

    uint64_t cfa;
    if (st.cfa_reg == DW_REG_RSP) cfa = rsp + st.cfa_off;
    else if (st.cfa_reg == DW_REG_RBP) cfa = rbp + st.cfa_off;
    else return false;

    if (cfa <= rsp || cfa >= 0xFFFFFFFF80000000ULL + 0x100000000ULL) return false;

    uint64_t nrip = 0;
    if (st.ra.k == RU_OFFSET) nrip = *(const uint64_t*)(cfa + st.ra.off);
    else if (st.ra.k == RU_SAME) nrip = rip;
    else return false;

    uint64_t nrbp;
    if (st.rbp.k == RU_OFFSET) nrbp = *(const uint64_t*)(cfa + st.rbp.off);
    else if (st.rbp.k == RU_VALOFF) nrbp = cfa + st.rbp.off;
    else if (st.rbp.k == RU_SAME) nrbp = rbp;
    else if (st.rbp.k == RU_REG) {
        if (st.rbp.reg == DW_REG_RBP) nrbp = rbp;
        else if (st.rbp.reg == DW_REG_RSP) nrbp = rsp;
        else nrbp = 0;
    } else nrbp = 0;

    // Validate: RSP must increase (we're moving up the stack)
    if (cfa <= rsp) return false;

    rip = nrip; rsp = cfa; rbp = nrbp;
    return true;
}

// ---------------------------------------------------------------------------
// Symbol lookup
// ---------------------------------------------------------------------------
static const char* lookup(uint64_t addr, uint64_t& off) {
    if (__kernel_symtab_count == 0) return nullptr;
    int64_t lo = 0, hi = (int64_t)(__kernel_symtab_count - 1);
    while (lo <= hi) {
        int64_t mid = lo + (hi - lo) / 2;
        uint64_t sa = __kernel_symtab_start[mid].addr;
        if (addr < sa) { hi = mid - 1; continue; }
        if ((uint64_t)mid == __kernel_symtab_count - 1 || addr < __kernel_symtab_start[mid + 1].addr) {
            off = addr - sa; return __kernel_symtab_start[mid].name;
        }
        lo = mid + 1;
    }
    return nullptr;
}

// ---------------------------------------------------------------------------
// Public API
// ---------------------------------------------------------------------------
int tori::stacktrace::collect(uint64_t rip, uint64_t rsp, uint64_t rbp,
                              uint64_t* out, int max_frames)
{
    int count = 0;
    for (int i = 0; i < max_frames; ++i) {
        if (count < max_frames) out[count++] = rip;
        if (!step(rip, rsp, rbp)) break;
        if (rip == 0) break;
    }
    return count;
}

int tori::stacktrace::collect_current(uint64_t* out, int max_frames) {
    uint64_t rbp, rsp;
    asm volatile("mov %%rbp, %0" : "=r"(rbp));
    asm volatile("mov %%rsp, %0" : "=r"(rsp));
    uint64_t rip = (uint64_t)__builtin_return_address(0);
    return collect(rip, rsp, rbp, out, max_frames);
}

const char* tori::stacktrace::resolve(uint64_t addr, uint64_t& offset) {
    const char* raw = lookup(addr, offset);
    if (!raw) return nullptr;
    static char demangle_buf[256];
    return tori::demangle::demangle(raw, demangle_buf, sizeof(demangle_buf));
}
