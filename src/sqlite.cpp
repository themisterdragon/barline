#include "sqlite.h"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace sqlite {

std::string Value::text() const {
    switch (type) {
    case INT: return std::to_string(i);
    case REAL: {
        char b[64];
        std::snprintf(b, sizeof b, "%g", d);
        return b;
    }
    case TEXT:
    case BLOB: return s;
    default: return "";
    }
}

double Value::num() const {
    switch (type) {
    case INT: return (double)i;
    case REAL: return d;
    case TEXT: return std::atof(s.c_str());
    default: return 0;
    }
}

namespace {

uint32_t be(const uint8_t *p, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; i++) v = (v << 8) | p[i];
    return v;
}

// a variable-length integer: up to 9 bytes; returns bytes used (0 when it runs off the end)
int varint(const uint8_t *p, const uint8_t *end, uint64_t &out) {
    out = 0;
    for (int i = 0; i < 9; i++) {
        if (p + i >= end) return 0;
        if (i == 8) {
            out = (out << 8) | p[i];
            return 9;
        }
        out = (out << 7) | (p[i] & 0x7F);
        if (!(p[i] & 0x80)) return i + 1;
    }
    return 0;
}

}  // namespace

const uint8_t *Db::page_ptr(uint32_t page) const {
    if (page < 1 || page > pages_) return nullptr;
    return (const uint8_t *)data_.data() + (size_t)(page - 1) * page_size_;
}

bool Db::open(const std::string &bytes, std::string *err) {
    data_ = bytes;
    const uint8_t *h = (const uint8_t *)data_.data();
    if (data_.size() < 100 || std::memcmp(h, "SQLite format 3\0", 16) != 0) {
        if (err) *err = "not a SQLite database";
        return false;
    }
    page_size_ = be(h + 16, 2);
    if (page_size_ == 1) page_size_ = 65536;
    if (page_size_ < 512 || page_size_ > 65536 || (page_size_ & (page_size_ - 1))) {
        if (err) *err = "a damaged database (page size)";
        return false;
    }
    usable_ = page_size_ - h[20];
    if (usable_ < 480) {
        if (err) *err = "a damaged database";
        return false;
    }
    if (be(h + 56, 4) > 1) {
        if (err) *err = "a database in UTF-16 (only UTF-8 is supported)";
        return false;
    }
    pages_ = (uint32_t)(data_.size() / page_size_);
    // the table list: sqlite_master, rooted on page 1 (type, name, tbl_name, rootpage, sql)
    int budget = 2000000;
    bool ok = walk(1, 0, [&](const std::vector<Value> &r) {
        if (r.size() >= 5 && r[0].text() == "table") tables_[r[1].text()] = {(uint32_t)r[3].num(), r[4].text()};
        return true;
    }, budget);
    if (!ok) {
        if (err) *err = "a damaged database (its table list)";
        return false;
    }
    return true;
}

// the payload of a table leaf cell, following overflow pages
bool Db::record(uint32_t page, size_t cell, std::vector<Value> &out) {
    const uint8_t *pg = page_ptr(page);
    const uint8_t *end = pg + page_size_;
    const uint8_t *p = pg + cell;
    if (p >= end) return false;
    uint64_t plen, rowid;
    int n = varint(p, end, plen);
    if (!n) return false;
    p += n;
    n = varint(p, end, rowid);
    if (!n) return false;
    p += n;
    if (plen > 64u * 1024 * 1024) return false;
    // how much is on this page (the file format's rule), the rest on a chain of overflow pages
    uint64_t U = usable_, X = U - 35, local = plen;
    if (plen > X) {
        uint64_t M = ((U - 12) * 32 / 255) - 23, K = M + ((plen - M) % (U - 4));
        local = K <= X ? K : M;
    }
    if (p + local > end) return false;
    std::string payload((const char *)p, (size_t)local);
    if (local < plen) {
        if (p + local + 4 > end) return false;
        uint32_t next = be(p + local, 4);
        int guard = 0;
        while (payload.size() < plen && next && guard++ < 100000) {
            const uint8_t *op = page_ptr(next);
            if (!op) return false;
            next = be(op, 4);
            size_t take = (size_t)std::min<uint64_t>(U - 4, plen - payload.size());
            payload.append((const char *)op + 4, take);
        }
        if (payload.size() < plen) return false;
    }
    // the record: a header of serial types, then the values
    const uint8_t *r = (const uint8_t *)payload.data(), *rend = r + payload.size();
    uint64_t hlen;
    n = varint(r, rend, hlen);
    if (!n || hlen > payload.size()) return false;
    const uint8_t *hp = r + n, *hend = r + hlen, *body = r + hlen;
    out.clear();
    while (hp < hend) {
        uint64_t t;
        n = varint(hp, hend, t);
        if (!n) return false;
        hp += n;
        Value v;
        size_t size = 0;
        if (t == 0) v.type = Value::NUL;
        else if (t <= 6) {
            static const size_t SZ[] = {0, 1, 2, 3, 4, 6, 8};
            size = SZ[t];
            if (body + size > rend) return false;
            int64_t x = (body[0] & 0x80) ? -1 : 0;
            for (size_t i = 0; i < size; i++) x = (int64_t)(((uint64_t)x << 8) | body[i]);
            v.type = Value::INT;
            v.i = x;
        } else if (t == 7) {
            size = 8;
            if (body + 8 > rend) return false;
            uint64_t bits = 0;
            for (int i = 0; i < 8; i++) bits = (bits << 8) | body[i];
            std::memcpy(&v.d, &bits, 8);
            v.type = Value::REAL;
        } else if (t == 8 || t == 9) {
            v.type = Value::INT;
            v.i = t == 9;
        } else if (t >= 12) {
            size = (size_t)((t - (t & 1 ? 13 : 12)) / 2);
            if (body + size > rend) return false;
            v.type = t & 1 ? Value::TEXT : Value::BLOB;
            v.s.assign((const char *)body, size);
        } else
            return false;   // 10, 11: reserved
        body += size;
        out.push_back(std::move(v));
    }
    (void)rowid;
    return true;
}

bool Db::walk(uint32_t page, int depth, const std::function<bool(const std::vector<Value> &)> &fn, int &budget) {
    if (depth > 40 || --budget < 0) return false;
    const uint8_t *pg = page_ptr(page);
    if (!pg) return false;
    size_t hdr = page == 1 ? 100 : 0;   // page 1 starts with the file header
    const uint8_t *h = pg + hdr;
    uint8_t type = h[0];
    uint32_t cells = be(h + 3, 2);
    size_t ptrs = hdr + (type == 0x05 ? 12 : 8);
    if (ptrs + cells * 2 > page_size_) return false;
    if (type == 0x0D) {   // a leaf: rows
        for (uint32_t i = 0; i < cells; i++) {
            size_t c = be(pg + ptrs + i * 2, 2);
            if (c >= page_size_) return false;
            std::vector<Value> row;
            if (!record(page, c, row)) return false;
            if (!fn(row)) return true;
        }
        return true;
    }
    if (type == 0x05) {   // interior: child pages, then the rightmost
        for (uint32_t i = 0; i < cells; i++) {
            size_t c = be(pg + ptrs + i * 2, 2);
            if (c + 4 > page_size_) return false;
            if (!walk(be(pg + c, 4), depth + 1, fn, budget)) return false;
        }
        return walk(be(h + 8, 4), depth + 1, fn, budget);
    }
    return false;   // an index page, or damage
}

std::vector<std::string> Db::columns(const std::string &table) {
    std::vector<std::pair<std::string, Value>> d = column_defs(table);
    std::vector<std::string> out;
    for (const auto &c : d) out.push_back(c.first);
    return out;
}

// each column and its DEFAULT (a row written before a column was added gets the default)
std::vector<std::pair<std::string, Value>> Db::column_defs(const std::string &table) {
    std::vector<std::pair<std::string, Value>> out;
    auto it = tables_.find(table);
    if (it == tables_.end()) return out;
    // the column list: between the first "(" and its match; each definition's first word
    const std::string &sql = it->second.second;
    size_t open = sql.find('(');
    if (open == std::string::npos) return out;
    int level = 0;
    std::string cur;
    std::vector<std::string> defs;
    for (size_t i = open + 1; i < sql.size(); i++) {
        char c = sql[i];
        if (c == '(') level++;
        if (c == ')') {
            if (level == 0) break;
            level--;
        }
        if (c == ',' && level == 0) {
            defs.push_back(cur);
            cur.clear();
        } else
            cur += c;
    }
    defs.push_back(cur);
    for (auto d : defs) {
        size_t i = 0;
        while (i < d.size() && std::isspace((unsigned char)d[i])) i++;
        d = d.substr(i);
        std::string name;
        if (!d.empty() && (d[0] == '`' || d[0] == '"' || d[0] == '[')) {
            char close = d[0] == '[' ? ']' : d[0];
            size_t e = d.find(close, 1);
            name = d.substr(1, e == std::string::npos ? std::string::npos : e - 1);
        } else {
            size_t e = 0;
            while (e < d.size() && !std::isspace((unsigned char)d[e])) e++;
            name = d.substr(0, e);
        }
        std::string up;
        for (char c : name) up += (char)std::toupper((unsigned char)c);
        if (up == "PRIMARY" || up == "UNIQUE" || up == "CHECK" || up == "FOREIGN" || up == "CONSTRAINT" || name.empty()) continue;
        Value dv;
        std::string low;
        for (char c : d) low += (char)std::tolower((unsigned char)c);
        size_t k = low.find(" default ");
        if (k != std::string::npos) {
            size_t v = k + 9;
            while (v < d.size() && std::isspace((unsigned char)d[v])) v++;
            if (v < d.size() && (d[v] == '\'' || d[v] == '"')) {
                size_t e = d.find(d[v], v + 1);
                dv.type = Value::TEXT;
                dv.s = d.substr(v + 1, e == std::string::npos ? std::string::npos : e - v - 1);
            } else {
                size_t e = v;
                while (e < d.size() && !std::isspace((unsigned char)d[e])) e++;
                std::string num = d.substr(v, e - v);
                if (!num.empty() && num != "NULL" && num != "null") {
                    dv.type = num.find('.') != std::string::npos ? Value::REAL : Value::INT;
                    dv.i = std::atoll(num.c_str());
                    dv.d = std::atof(num.c_str());
                }
            }
        }
        out.push_back({name, dv});
    }
    return out;
}

bool Db::rows(const std::string &table, const std::function<void(const std::map<std::string, Value> &)> &fn, std::string *err) {
    auto it = tables_.find(table);
    if (it == tables_.end()) {
        if (err) *err = "no table " + table;
        return false;
    }
    auto cols = column_defs(table);
    int budget = 2000000;
    bool ok = walk(it->second.first, 0, [&](const std::vector<Value> &r) {
        std::map<std::string, Value> m;
        for (size_t i = 0; i < cols.size(); i++) m[cols[i].first] = i < r.size() ? r[i] : cols[i].second;
        fn(m);
        return true;
    }, budget);
    if (!ok && err) *err = "the " + table + " table is damaged";
    return ok;
}

}  // namespace sqlite
