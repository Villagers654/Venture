// bionic fts(3) (libc/include/fts.h, from OpenBSD): fts_open, fts_read,
// fts_children, fts_set and fts_close over the guest VFS. The traversal never
// changes the working directory: fts_accpath is the full path, as with
// FTS_NOCHDIR. Entries returned by fts_read stay valid until fts_close.
#include "vfs.h"
#include "hle/libc/libc_a.h"
#include <algorithm>
#include <cstddef>
#include <cstring>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace venture::hle::libc {
namespace {
// LP64 bionic layouts.
struct GuestFtsent {
  GuestAddr cycle, parent, link;
  std::int64_t number;
  GuestAddr pointer, accpath, path;
  std::int32_t error, symfd;
  std::uint64_t pathlen, namelen, ino, dev;
  std::uint32_t nlink;
  std::int16_t level;
  std::uint16_t info, flags, instr;
  GuestAddr statp;
  char name[1];
};
static_assert(offsetof(GuestFtsent, pathlen) == 64 && offsetof(GuestFtsent, nlink) == 96 &&
              offsetof(GuestFtsent, level) == 100 && offsetof(GuestFtsent, statp) == 112 &&
              offsetof(GuestFtsent, name) == 120);
struct GuestFts {
  GuestAddr cur, child, array;
  std::uint64_t dev;
  GuestAddr path;
  std::int32_t rfd;
  std::uint64_t pathlen;
  std::int32_t nitems;
  GuestAddr compar;
  std::int32_t options;
};
static_assert(offsetof(GuestFts, pathlen) == 48 && offsetof(GuestFts, compar) == 64 && sizeof(GuestFts) == 80);

constexpr int kComFollow = 0x1, kLogical = 0x2, kPhysical = 0x10, kSeeDot = 0x20, kXdev = 0x40, kOptionMask = 0xff;
enum Info : std::uint16_t { D = 1, DC = 2, DNR = 4, DOT = 5, DP = 6, ERR = 7, F = 8, NS = 10, SL = 12, SLNONE = 13 };
enum Instr : std::uint16_t { AGAIN = 1, FOLLOW = 2, NOINSTR = 3, SKIP = 4 };

struct Node {
  GuestFtsent* entry = nullptr;  // guest heap: header, name, path and stat in one block
  Node* parent = nullptr;
  std::vector<std::unique_ptr<Node>> children;
  bool children_read = false;
  std::size_t index = 0;  // in the parent's children (or the roots)
  std::string path;
  ~Node() { if (entry) deallocate(entry); }
  Stat* stat() const { return reinterpret_cast<Stat*>(g2h<void>(entry->statp)); }
};
struct Walk {
  GuestFts* fts = nullptr;
  int options = 0;
  GuestAddr compar = 0;
  Node root_parent;  // level -1
  Node* current = nullptr;
  bool started = false;
};
std::mutex walks_mutex;
std::map<GuestFts*, std::unique_ptr<Walk>> walks;

void clear_errno() {
  if (auto* t = GuestThread::current()) t->set_errno(0);
}
bool is_dir(const Stat& s) { return (s.mode & 0170000) == 0040000; }
bool is_link(const Stat& s) { return (s.mode & 0170000) == 0120000; }
// Classifies `node` from its path, as fts_stat() does.
void classify(Walk& walk, Node& node, bool follow) {
  auto& e = *node.entry;
  auto& s = *node.stat();
  e.error = 0;
  int r = path_stat(node.path.c_str(), &s, at_cwd, follow);
  if (r && follow && !path_stat(node.path.c_str(), &s, at_cwd, false)) {
    e.info = SLNONE;
  } else if (r) {
    s = {};
    e.error = -r;
    e.info = NS;
  } else if (is_dir(s)) {
    e.info = D;
    for (auto* up = node.parent; up && up != &walk.root_parent; up = up->parent)
      if (up->stat()->dev == s.dev && up->stat()->ino == s.ino) {
        e.info = DC;
        e.cycle = h2g(up->entry);
        break;
      }
    const auto& name = std::string_view(e.name, e.namelen);
    if (node.parent != &walk.root_parent && (name == "." || name == "..")) e.info = DOT;
  } else {
    e.info = is_link(s) ? SL : F;
  }
  e.ino = s.ino;
  e.dev = s.dev;
  e.nlink = s.nlink;
}
std::unique_ptr<Node> make_node(Walk& walk, Node* parent, std::string_view name, std::string path) {
  auto node = std::make_unique<Node>();
  const auto header = offsetof(GuestFtsent, name) + name.size() + 1;
  const auto path_at = (header + 7) & ~std::size_t(7);
  const auto stat_at = (path_at + path.size() + 1 + 15) & ~std::size_t(15);
  auto* block = static_cast<std::byte*>(allocate(stat_at + sizeof(Stat)));
  if (!block) return nullptr;
  std::memset(block, 0, stat_at + sizeof(Stat));
  node->entry = reinterpret_cast<GuestFtsent*>(block);
  node->parent = parent;
  node->path = std::move(path);
  auto& e = *node->entry;
  std::memcpy(e.name, name.data(), name.size());
  std::memcpy(block + path_at, node->path.c_str(), node->path.size() + 1);
  e.namelen = name.size();
  e.path = e.accpath = h2g(block + path_at);
  e.pathlen = node->path.size();
  e.statp = h2g(block + stat_at);
  e.parent = parent->entry ? h2g(parent->entry) : 0;
  e.level = std::int16_t(parent->entry ? parent->entry->level + 1 : 0);
  e.instr = NOINSTR;
  e.symfd = -1;
  const bool follow = (walk.options & kLogical) || (e.level == 0 && (walk.options & kComFollow));
  classify(walk, *node, follow);
  return node;
}
// Links a child list in order (fts_link) and applies the comparison.
void order(Walk& walk, std::vector<std::unique_ptr<Node>>& list) {
  if (walk.compar && list.size() > 1)
    std::stable_sort(list.begin(), list.end(), [&](const auto& a, const auto& b) {
      GuestAddr left = h2g(a->entry), right = h2g(b->entry);
      return static_cast<std::int32_t>(GuestThread::current()->call(walk.compar, {h2g(&left), h2g(&right)})) < 0;
    });
  for (std::size_t i = 0; i < list.size(); ++i) {
    list[i]->index = i;
    list[i]->entry->link = i + 1 < list.size() ? h2g(list[i + 1]->entry) : 0;
  }
}
// Reads a directory's children once; false (errno in the entry) if it cannot be read.
bool read_children(Walk& walk, Node& dir) {
  if (dir.children_read) return true;
  dir.children_read = true;
  const auto opened = fd_open(dir.path.c_str(), 0200000 | o_cloexec);
  if (opened < 0) {
    dir.entry->error = static_cast<std::int32_t>(-opened);
    return false;
  }
  const int fd = static_cast<int>(opened);
  std::vector<Dirent> entries;
  int r = FdTable::get().file(fd)->entries(entries);
  FdTable::get().close(fd);
  if (r) {
    dir.entry->error = -r;
    return false;
  }
  for (auto& d : entries) {
    std::string_view name(d.name);
    if ((name == "." || name == "..") && !(walk.options & kSeeDot)) continue;
    auto path = dir.path + (dir.path.ends_with('/') ? "" : "/") + std::string(name);
    if (auto child = make_node(walk, &dir, name, std::move(path))) dir.children.push_back(std::move(child));
  }
  order(walk, dir.children);
  return true;
}
GuestFtsent* show(Walk& walk, Node* node) {
  walk.current = node;
  walk.fts->cur = node ? h2g(node->entry) : 0;
  walk.fts->path = node ? node->entry->path : 0;
  walk.fts->pathlen = node ? node->path.size() : 0;
  if (!node) clear_errno();
  return node ? node->entry : nullptr;
}
Walk* find(GuestFts* fts) {
  std::lock_guard lock(walks_mutex);
  auto it = walks.find(fts);
  return it == walks.end() ? nullptr : it->second.get();
}

GuestFts* fts_open_fn(const GuestAddr* argv, int options, GuestAddr compar) {
  if (!argv || (options & ~kOptionMask) || !(options & (kLogical | kPhysical))) {
    failure(err::einval);
    return nullptr;
  }
  auto walk = std::make_unique<Walk>();
  walk->options = options;
  walk->compar = compar;
  for (auto* arg = argv; *arg; ++arg) {
    const char* path = g2h<const char>(*arg);
    if (!*path) {
      failure(err::enoent);
      return nullptr;
    }
    std::string_view name(path);
    if (auto root = make_node(*walk, &walk->root_parent, name, std::string(name)))
      walk->root_parent.children.push_back(std::move(root));
    else {
      failure(err::enomem);
      return nullptr;
    }
  }
  order(*walk, walk->root_parent.children);
  walk->root_parent.children_read = true;
  auto* fts = static_cast<GuestFts*>(allocate(sizeof(GuestFts)));
  if (!fts) {
    failure(err::enomem);
    return nullptr;
  }
  *fts = {};
  fts->compar = compar;
  fts->options = options;
  fts->rfd = -1;
  if (!walk->root_parent.children.empty()) fts->dev = walk->root_parent.children[0]->entry->dev;
  walk->fts = fts;
  std::lock_guard lock(walks_mutex);
  walks[fts] = std::move(walk);
  return fts;
}
GuestFtsent* fts_read_fn(GuestFts* fts) {
  auto* walk = find(fts);
  if (!walk) {
    failure(err::einval);
    return nullptr;
  }
  auto& roots = walk->root_parent.children;
  if (!walk->started) {
    walk->started = true;
    return show(*walk, roots.empty() ? nullptr : roots.front().get());
  }
  Node* node = walk->current;
  if (!node) return show(*walk, nullptr);
  auto& e = *node->entry;
  const auto instr = e.instr;
  e.instr = NOINSTR;
  if (instr == AGAIN) {
    classify(*walk, *node, (walk->options & kLogical) || (e.level == 0 && (walk->options & kComFollow)));
    return show(*walk, node);
  }
  if (instr == FOLLOW && (e.info == SL || e.info == SLNONE)) {
    classify(*walk, *node, true);
    return show(*walk, node);
  }
  // A directory just shown before its contents: descend unless skipped.
  if (e.info == D && instr != SKIP &&
      !((walk->options & kXdev) && e.level > 0 && e.dev != walk->fts->dev)) {
    if (!read_children(*walk, *node)) {
      e.info = DNR;
      return show(*walk, node);
    }
    if (!node->children.empty()) return show(*walk, node->children.front().get());
    e.info = DP;
    return show(*walk, node);
  }
  // Next sibling, or the parent after its contents.
  for (;;) {
    auto& list = node->parent->children;
    if (node->index + 1 < list.size()) return show(*walk, list[node->index + 1].get());
    Node* parent = node->parent;
    if (parent == &walk->root_parent) return show(*walk, nullptr);
    parent->entry->info = DP;
    return show(*walk, parent);
  }
}
GuestFtsent* fts_children_fn(GuestFts* fts, int options) {
  auto* walk = find(fts);
  if (!walk || (options & ~0x100)) {
    failure(err::einval);
    return nullptr;
  }
  clear_errno();
  auto& roots = walk->root_parent.children;
  if (!walk->started) return roots.empty() ? nullptr : roots.front()->entry;
  Node* node = walk->current;
  if (!node || node->entry->info != D) return nullptr;
  if (!read_children(*walk, *node) || node->children.empty()) return nullptr;
  walk->fts->child = h2g(node->children.front()->entry);
  return node->children.front()->entry;
}
int fts_set_fn(GuestFts* fts, GuestFtsent* entry, int instr) {
  if (!fts || !entry || (instr != 0 && instr != AGAIN && instr != FOLLOW && instr != NOINSTR && instr != SKIP))
    return failure(err::einval);
  entry->instr = static_cast<std::uint16_t>(instr ? instr : NOINSTR);
  return 0;
}
int fts_close_fn(GuestFts* fts) {
  std::unique_ptr<Walk> walk;
  {
    std::lock_guard lock(walks_mutex);
    auto it = walks.find(fts);
    if (it == walks.end()) return failure(err::einval);
    walk = std::move(it->second);
    walks.erase(it);
  }
  walk.reset();
  deallocate(fts);
  return 0;
}
}  // namespace
void register_fts(HleLibrary& lib) {
  if (process_config().isa() != GuestIsa::a64) return;  // the LP64 layouts above
#define B(n, f) VENTURE_LIBC_BIND(lib, n, f)
  B("fts_open", fts_open_fn);
  B("fts_read", fts_read_fn);
  B("fts_children", fts_children_fn);
  B("fts_set", fts_set_fn);
  B("fts_close", fts_close_fn);
#undef B
}
}  // namespace venture::hle::libc
