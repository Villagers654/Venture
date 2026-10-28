// An exported data symbol also tests object lookup and pointer relocations.
extern "C" {
int vtest_dl_value = 41;
int vtest_dl_add(int value) { return vtest_dl_value + value; }
}
