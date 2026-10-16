// The few things Linux, macOS, and Windows do differently (files, folders, the one-copy lock, the
// clock, the text editor), so the rest of Barline is written once.
#pragma once
#include <ctime>
#include <string>
#include <vector>

namespace sys {

bool make_dir(const std::string &path, int mode = 0755);      // false if it isn't there afterwards
bool is_dir(const std::string &path);
bool exists(const std::string &path);
long long file_size(const std::string &path);                 // -1 if it isn't there
std::vector<std::string> list_dir(const std::string &dir);    // names (no . or ..), unsorted
// write a whole file that only this user can read (settings, songs), safely: a temporary file,
// then swapped in, so a crash never leaves half a file
bool write_private(const std::string &path, const std::string &text);
bool append_private(const std::string &path, const std::string &text);
bool replace_file(const std::string &from, const std::string &to);   // rename, over the top of `to`
std::string exe_path();                                       // this program's own file
std::string host_name();                                      // "my-laptop" (no domain)
bool is_terminal();                                           // both ends are a terminal (else: the window)
// Windows: started by a double-click (the console is its own, new): it's let go of, and true
bool double_clicked();
void local_time(std::time_t t, std::tm *out);

// one Barline at a time: holds the lock while this program runs. false if another copy has it.
bool take_lock(const std::string &path);
bool lock_is_held(const std::string &path);                   // another copy is running (doesn't take it)

// the lyrics (or any text) in a text editor; blocks until it's closed. false if none could run.
bool edit_text_file(const std::string &path);

// timers: Windows' are 15 ms coarse unless asked (MIDI clock needs about 1 ms)
void fine_timers();
void audio_thread_priority();   // as high as this user may (sound and MIDI threads)

}  // namespace sys
