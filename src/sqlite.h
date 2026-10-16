// Reading a SQLite database file (read-only, from memory): just enough to walk a table's rows, for
// importing a set list app's database export. Follows the published file format
// (sqlite.org/fileformat.html); damaged or unexpected files are refused, never trusted.
#pragma once
#include <cstdint>
#include <functional>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace sqlite {

struct Value {
    enum Type { NUL, INT, REAL, TEXT, BLOB } type = NUL;
    int64_t i = 0;
    double d = 0;
    std::string s;   // TEXT or BLOB
    std::string text() const;   // anything as text ("" for NULL)
    double num() const;
};

class Db {
public:
    // false (and err) when it isn't a SQLite file this can read
    bool open(const std::string &bytes, std::string *err);
    // a table's column names, from its CREATE TABLE statement
    std::vector<std::string> columns(const std::string &table);
    std::vector<std::pair<std::string, Value>> column_defs(const std::string &table);
    // every row of a table, as column name -> value; false when the table is missing or damaged
    bool rows(const std::string &table, const std::function<void(const std::map<std::string, Value> &)> &fn, std::string *err = nullptr);

private:
    const std::string *d_ = nullptr;
    std::string data_;
    uint32_t page_size_ = 0, usable_ = 0, pages_ = 0;
    std::map<std::string, std::pair<uint32_t, std::string>> tables_;   // name -> (root page, CREATE sql)
    bool walk(uint32_t page, int depth, const std::function<bool(const std::vector<Value> &)> &fn, int &budget);
    bool record(uint32_t page, size_t cell, std::vector<Value> &out);
    const uint8_t *page_ptr(uint32_t page) const;
};

}  // namespace sqlite
