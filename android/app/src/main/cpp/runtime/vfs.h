#pragma once
// Windows path namespace -> Android host paths.
//
//   C:\NFSU2\...           -> <game_dir>/...            (the user's install)
//   C:\Users\Player\...    -> <user_dir>/profile/...    (saves, AppData, Documents)
//   X:\...                 -> <user_dir>/drive_x/...    (anything else)
//
// Lookups are case-insensitive per path component, as on Windows.
#include <string>

namespace vfs {

void init(const std::string& game_dir, const std::string& user_dir);

// Absolute, normalized Windows path ("C:\A\B") for a guest path, applying the
// current directory to relative paths.
std::string full_win_path(const std::string& guest_path);

// Host path for a guest path. Existing components are matched
// case-insensitively; a missing tail is appended as given (for creation).
std::string to_host(const std::string& guest_path);

std::string cwd();
bool set_cwd(const std::string& guest_path);

constexpr const char* kUserProfileWin = "C:\\Users\\Player";

// Windows-style wildcard match ('*' and '?'), case-insensitive.
bool wildcard_match(const char* pattern, const char* name);

}  // namespace vfs
