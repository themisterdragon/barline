// Importing songs from a set list app: its spreadsheet export (CSV), with titles,
// artists, keys, tempos, notes, and lyrics. Songs that are already here (same name and artist) are
// updated rather than doubled; their beats and loop settings are kept.
#pragma once
#include <string>

#include "songs.h"

namespace setlist_import {

struct Result {
    int songs = 0, setlists = 0;
    std::string error;
};

Result import(const std::string &data, songs::Library &lib);

}  // namespace setlist_import
