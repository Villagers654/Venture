// Freestanding synthetic ABI fixture. No engine, SDK or game source.
class ICVar {
 public:
  explicit ICVar(float initial) : value(initial) {}
  __attribute__((visibility("hidden"))) virtual float GetFVal() const;
  float value;
};
float ICVar::GetFVal() const { return value; }
static ICVar cap(72), fixed(0);
static int writes, mode, queries;
static const char* line;
static bool equal(const char* a, const char* b) {
  while (*a && *a == *b) { ++a; ++b; }
  return *a == *b;
}
class CXConsole {
 public:
  void Update();
  ICVar* GetCVar(const char*) const;
  void ExecuteString(const char*, bool, bool);
  void DisplayVarValue(ICVar*);
  bool GetLineNo(int, char*, int) const;
};
static CXConsole console;
ICVar* CXConsole::GetCVar(const char* name) const {
  if (mode == 4) return nullptr;
  return equal(name, "sys_MaxFPS") ? &cap : equal(name, "t_FixedStep") ? &fixed : nullptr;
}
void CXConsole::ExecuteString(const char* command, bool, bool) {
  if (equal(command, "sys_MaxFPS -1")) { cap.value = -1; ++writes; }
  if (equal(command, "sys_MaxFPS 72.000000")) { cap.value = 72; ++writes; }
}
void CXConsole::DisplayVarValue(ICVar* variable) {
  ++queries;
  if (mode == 5) { line = "unrelated or malformed console output"; return; }
  if (variable == &fixed) line = fixed.value == 0 ? "$3t_FixedStep = $60 $5[]$4" : "$3t_FixedStep = $60.013888889 $5[]$4";
  else line = cap.value < 0 ? "$3sys_MaxFPS = $6-1 $5[]$4" : cap.value == 0 ? "$3sys_MaxFPS = $60 $5[]$4" : "$3sys_MaxFPS = $672 $5[]$4";
}
bool CXConsole::GetLineNo(int, char* out, int capacity) const {
  if (!line || capacity <= 0) return false;
  int i=0;for (;i+1<capacity&&line[i];++i)out[i]=line[i];out[i]=0;
  return true;
}
asm(".text\n.global _ZN9CXConsole6UpdateEv\n.type _ZN9CXConsole6UpdateEv,%function\n"
    "_ZN9CXConsole6UpdateEv:\nnop\nadd x0,x0,#7\nret\n"
    ".size _ZN9CXConsole6UpdateEv,.-_ZN9CXConsole6UpdateEv\n");
extern "C" void fixture_configure(int selection) {
  mode = selection;
  fixed.value = selection == 1 || selection == 6 ? 1.f/72 : 0;
  cap.value = selection == 2 ? 0 : selection == 3 || selection == 6 ? -1 : 72;
  writes = 0;
  queries = 0;
}
extern "C" CXConsole* fixture_console() { return &console; }
extern "C" float fixture_cap() { return cap.value; }
extern "C" float fixture_fixed_step() { return fixed.value; }
extern "C" int fixture_writes() { return writes; }
extern "C" int fixture_queries() { return queries; }
