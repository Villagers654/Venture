// system(): the commands an app runs through Android's /system/bin/sh.
//
// A guest cannot start host processes, but apps and their mods run simple
// shell commands with system() and expect them to work as on a headset, where
// sh runs toybox: beatsaber-hook's mkpath, used by many Beat Saber mods to make
// their folders, is system("mkdir -p <path>"). This runs such command lines on
// the guest's own file system: commands joined by ";", "&&" and "||", words in
// quotes or with backslash escapes, output sent to /dev/null, and the toybox
// file commands mkdir, rmdir, rm, touch, true and false. Anything else (pipes,
// expansion, other programs) fails as sh does for a missing program, with
// status 127.
#include "shell.h"

#include "libc_parts.h"
#include "vfs.h"
#include "common/log.h"

#include <filesystem>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <system_error>
#include <vector>

namespace venture::hle::libc {
namespace {
enum class Join { always, on_success, on_failure };
struct Command {
  std::vector<std::string> words;
  Join join = Join::always;  // how this command follows the previous one
  bool unsupported = false;
};

// Splits a command line into commands. False when it uses shell syntax this
// does not run (pipes, expansion, substitution, other redirections).
bool parse(std::string_view line, std::vector<Command>* out) {
  out->assign(1, {});
  std::string word;
  bool in_word = false;
  auto finish = [&] {
    if (in_word) out->back().words.push_back(std::move(word));
    word.clear();
    in_word = false;
  };
  for (std::size_t i = 0; i < line.size(); ++i) {
    const char c = line[i];
    if (c == '\'') {
      const auto end = line.find('\'', i + 1);
      if (end == std::string_view::npos) return false;
      word.append(line.substr(i + 1, end - i - 1));
      in_word = true;
      i = end;
    } else if (c == '"') {
      in_word = true;
      for (++i; i < line.size() && line[i] != '"'; ++i) {
        if (line[i] == '$' || line[i] == '`') return false;
        if (line[i] == '\\' && i + 1 < line.size() && std::string_view("\"\\$`").find(line[i + 1]) != std::string_view::npos)
          ++i;
        word.push_back(line[i]);
      }
      if (i == line.size()) return false;
    } else if (c == '\\') {
      if (++i == line.size()) return false;
      if (line[i] != '\n') word.push_back(line[i]), in_word = true;
    } else if (c == ' ' || c == '\t' || c == '\n') {
      finish();
    } else if (c == ';' || ((c == '&' || c == '|') && i + 1 < line.size() && line[i + 1] == c)) {
      finish();
      const auto join = c == ';' ? Join::always : c == '&' ? Join::on_success : Join::on_failure;
      if (c != ';') ++i;
      if (out->back().words.empty()) return false;
      out->push_back({.join = join});
    } else if (c == '>' || ((c == '1' || c == '2') && !in_word && i + 1 < line.size() && line[i + 1] == '>')) {
      // Output redirections to /dev/null, or of one standard stream to the
      // other: the commands write nothing a caller reads.
      finish();
      if (c != '>') ++i;
      if (++i < line.size() && line[i] == '&') {
        if (++i == line.size() || (line[i] != '1' && line[i] != '2')) return false;
        continue;
      }
      while (i < line.size() && (line[i] == ' ' || line[i] == '\t')) ++i;
      if (line.substr(i, 9) != "/dev/null") return false;
      i += 8;
    } else if (std::string_view("|&<$`()*?[{}~#").find(c) != std::string_view::npos) {
      return false;
    } else {
      word.push_back(c);
      in_word = true;
    }
  }
  finish();
  if (out->back().words.empty()) {
    if (out->size() > 1 && out->back().join != Join::always) return false;
    out->pop_back();
  }
  return true;
}

// Options: each word starting with "-" before the first operand ("--" ends them).
std::string options(std::vector<std::string>& words, std::string_view allowed, std::string* value = nullptr) {
  std::string found;
  std::size_t i = 1;
  for (; i < words.size() && words[i].size() > 1 && words[i][0] == '-'; ++i) {
    if (words[i] == "--") { ++i; break; }
    for (std::size_t k = 1; k < words[i].size(); ++k) {
      const char o = words[i][k];
      if (allowed.find(o) == std::string_view::npos) return "?";
      found.push_back(o);
      if (o == 'm' && value) {  // -m MODE or -mMODE
        *value = k + 1 < words[i].size() ? words[i].substr(k + 1) : i + 1 < words.size() ? words[++i] : "";
        break;
      }
    }
  }
  words.erase(words.begin() + 1, words.begin() + static_cast<std::ptrdiff_t>(i));
  return found;
}
bool is_directory(const std::string& path) {
  Stat s{};
  return path_stat(path.c_str(), &s) == 0 && (s.mode & 0170000) == 0040000;
}
int mutate(int op, const std::string& path, std::uint32_t mode = 0) {
  MappedPath m;
  if (const int r = resolve_path(path, at_cwd, &m)) return r;
  if (m.readonly || m.device) {
    Stat s{};
    return op == 1 && path_stat(m, &s) == 0 ? -err::eexist : -err::erofs;
  }
  return host::mutate(op, m, nullptr, mode);
}
int fail(std::string_view command, const std::string& path, int error) {
  log::warn("sh", "{}: {}: {}", command, path, error_string(-error));
  return 1;
}
int usage(std::string_view command) {
  log::warn("sh", "{}: options it does not take", command);
  return 1;
}

// A chmod-style mode, octal ("0755") or symbolic ("+rw", "u=rwx,go-w"),
// applied to `base` as toybox's string_to_mode does; nullopt when invalid.
std::optional<std::uint32_t> parse_mode(std::string_view text, std::uint32_t base) {
  if (text.empty()) return std::nullopt;
  if (text.find_first_not_of("01234567") == std::string_view::npos)
    return text.size() <= 4 ? std::optional(static_cast<std::uint32_t>(std::stoul(std::string(text), nullptr, 8))) : std::nullopt;
  std::uint32_t mode = base;
  for (std::size_t i = 0; i < text.size();) {
    std::uint32_t who = 0;
    for (; i < text.size() && std::string_view("ugoa").find(text[i]) != std::string_view::npos; ++i)
      who |= text[i] == 'u' ? 04700u : text[i] == 'g' ? 02070u : text[i] == 'o' ? 01007u : 07777u;
    if (!who) who = 07777;
    do {
      if (i == text.size() || std::string_view("+-=").find(text[i]) == std::string_view::npos) return std::nullopt;
      const char op = text[i++];
      std::uint32_t bits = 0;
      for (; i < text.size() && text[i] != ',' && std::string_view("+-=").find(text[i]) == std::string_view::npos; ++i) {
        switch (text[i]) {
          case 'r': bits |= 0444; break;
          case 'w': bits |= 0222; break;
          case 'x': case 'X': bits |= 0111; break;
          case 's': bits |= 06000; break;
          case 't': bits |= 01000; break;
          default: return std::nullopt;
        }
      }
      bits &= who;
      if (op == '+') mode |= bits;
      else if (op == '-') mode &= ~bits;
      else mode = (mode & ~who) | bits;
    } while (i < text.size() && text[i] != ',');
    if (i < text.size()) ++i;  // the comma
  }
  return mode & 07777;
}

int mkdir(std::vector<std::string> words) {
  std::string mode_text;
  const auto flags = options(words, "pm", &mode_text);
  if (flags == "?" || words.size() < 2) return usage("mkdir");
  std::uint32_t mode = 0777;
  if (flags.find('m') != std::string::npos) {
    const auto parsed = parse_mode(mode_text, 0777);
    if (!parsed) return usage("mkdir");
    mode = *parsed;
  }
  const bool parents = flags.find('p') != std::string::npos;
  int status = 0;
  for (std::size_t i = 1; i < words.size(); ++i) {
    const auto& path = words[i];
    if (!parents) {
      if (const int r = mutate(1, path, mode)) status = fail("mkdir", path, r);
      continue;
    }
    // Each missing component in turn, as toybox's mkdir -p.
    for (std::size_t end = path.find('/', 1);; end = path.find('/', end + 1)) {
      const auto part = path.substr(0, end);
      if (!part.empty() && part.back() != '/' && !is_directory(part)) {
        const int r = mutate(1, part, end == std::string::npos ? mode : 0777);
        if (r && !(r == -err::eexist && is_directory(part))) {
          status = fail("mkdir", part, r);
          break;
        }
      }
      if (end == std::string::npos) break;
    }
  }
  return status;
}
int remove_tree(const std::string& path) {
  MappedPath m;
  if (const int r = resolve_path(path, at_cwd, &m)) return r;
  if (m.readonly || m.device) return -err::erofs;
  std::error_code error;
  std::filesystem::remove_all(m.host, error);
  return error ? -err::eio : 0;
}
int rm(std::vector<std::string> words) {
  const auto flags = options(words, "rRf");
  if (flags == "?") return usage("rm");
  const bool recursive = flags.find_first_of("rR") != std::string::npos, force = flags.find('f') != std::string::npos;
  if (words.size() < 2) return force ? 0 : 1;
  int status = 0;
  for (std::size_t i = 1; i < words.size(); ++i) {
    const auto& path = words[i];
    Stat s{};
    if (path_stat(path.c_str(), &s, at_cwd, false)) {
      if (!force) status = fail("rm", path, -err::enoent);
      continue;
    }
    const bool directory = (s.mode & 0170000) == 0040000;
    if (directory && !recursive) { status = fail("rm", path, -err::eisdir); continue; }
    if (const int r = directory ? remove_tree(path) : mutate(2, path)) status = fail("rm", path, r);
  }
  return status;
}
int rmdir(std::vector<std::string> words) {
  if (options(words, "") == "?" || words.size() < 2) return usage("rmdir");
  int status = 0;
  for (std::size_t i = 1; i < words.size(); ++i)
    if (const int r = mutate(3, words[i])) status = fail("rmdir", words[i], r);
  return status;
}
int touch(std::vector<std::string> words) {
  if (options(words, "c") == "?" || words.size() < 2) return usage("touch");
  int status = 0;
  for (std::size_t i = 1; i < words.size(); ++i) {
    const auto fd = fd_open(words[i].c_str(), 0100 | 1, 0644);  // O_CREAT | O_WRONLY
    if (fd < 0) status = fail("touch", words[i], static_cast<int>(fd));
    else FdTable::get().close(static_cast<int>(fd));
  }
  return status;
}
int run(const Command& command) {
  auto words = command.words;
  auto name = words[0];
  if (const auto slash = name.rfind('/'); slash != std::string::npos &&
      (name.starts_with("/system/bin/") || name.starts_with("/bin/") || name.starts_with("/system/xbin/")))
    name = name.substr(slash + 1);
  if (name == "true" || name == ":") return 0;
  if (name == "false") return 1;
  if (name == "mkdir") return mkdir(std::move(words));
  if (name == "rm") return rm(std::move(words));
  if (name == "rmdir") return rmdir(std::move(words));
  if (name == "touch") return touch(std::move(words));
  return 127;
}
void report_unsupported(std::string_view line) {
  static std::mutex mutex;
  static std::set<std::string, std::less<>> reported;
  std::lock_guard lock(mutex);
  if (reported.size() < 64 && reported.emplace(line).second)
    log::warn("sh", "cannot run \"{}\": only simple file commands (mkdir, rm, rmdir, touch) run without a shell", line);
}
}  // namespace

int run_shell_command(std::string_view line) {
  std::vector<Command> commands;
  if (!parse(line, &commands)) {
    report_unsupported(line);
    return 127;
  }
  int status = 0;
  for (const auto& command : commands) {
    if ((command.join == Join::on_success && status != 0) || (command.join == Join::on_failure && status == 0)) continue;
    status = run(command);
    if (status == 127) report_unsupported(line);
  }
  return status;
}
}  // namespace venture::hle::libc
