#include "java/natives/sqlite_bridge.h"

#include <jni.h>

#include "core/os_quit.h"
#include <cstdint>
#include <mutex>
#include <set>
#include <string>
#include <type_traits>
#include <vector>

#include "java/jvm.h"
#include "platform/sqlite_library.h"

// AOSP android_database_SQLiteConnection.cpp over the system SQLite's C API.
namespace venture::java {
namespace {
struct sqlite3;
struct sqlite3_stmt;
constexpr int sqlite_ok = 0, sqlite_cantopen = 14, sqlite_row = 100, sqlite_done = 101;
constexpr int type_integer = 1, type_float = 2, type_text = 3, type_blob = 4;
constexpr int busy_timeout_ms = 2500;  // SQLiteConnection BUSY_TIMEOUT_MS
using Destructor = void (*)(void*);
const auto transient = reinterpret_cast<Destructor>(static_cast<std::intptr_t>(-1));  // SQLITE_TRANSIENT

struct Api {
  int (*cacheflush)(sqlite3*) = nullptr;
  int (*open_v2)(const char*, sqlite3**, int, const char*);
  int (*close_v2)(sqlite3*);
  int (*extended_result_codes)(sqlite3*, int);
  int (*busy_timeout)(sqlite3*, int);
  int (*prepare16_v2)(sqlite3*, const void*, int, sqlite3_stmt**, const void**);
  int (*finalize)(sqlite3_stmt*);
  int (*step)(sqlite3_stmt*);
  int (*bind_parameter_count)(sqlite3_stmt*);
  int (*bind_null)(sqlite3_stmt*, int);
  int (*bind_int64)(sqlite3_stmt*, int, long long);
  int (*bind_double)(sqlite3_stmt*, int, double);
  int (*bind_text16)(sqlite3_stmt*, int, const void*, int, Destructor);
  int (*bind_blob)(sqlite3_stmt*, int, const void*, int, Destructor);
  int (*column_count)(sqlite3_stmt*);
  const void* (*column_name16)(sqlite3_stmt*, int);
  int (*column_type)(sqlite3_stmt*, int);
  long long (*column_int64)(sqlite3_stmt*, int);
  double (*column_double)(sqlite3_stmt*, int);
  const void* (*column_text16)(sqlite3_stmt*, int);
  int (*column_bytes16)(sqlite3_stmt*, int);
  const void* (*column_blob)(sqlite3_stmt*, int);
  int (*column_bytes)(sqlite3_stmt*, int);
  int (*changes)(sqlite3*);
  long long (*last_insert_rowid)(sqlite3*);
  int (*extended_errcode)(sqlite3*);
  const void* (*errmsg16)(sqlite3*);
  const char* (*errstr)(int);
};

const Api* api() {
  static const Api* loaded = [] () -> const Api* {
    static Api a;
    bool complete = true;
    auto bind = [&](auto& field, const char* name) {
      field = reinterpret_cast<std::remove_reference_t<decltype(field)>>(platform::sqlite_symbol(name));
      complete = complete && field;
    };
    bind(a.open_v2, "sqlite3_open_v2"); bind(a.close_v2, "sqlite3_close_v2");
    bind(a.extended_result_codes, "sqlite3_extended_result_codes"); bind(a.busy_timeout, "sqlite3_busy_timeout");
    bind(a.prepare16_v2, "sqlite3_prepare16_v2"); bind(a.finalize, "sqlite3_finalize"); bind(a.step, "sqlite3_step");
    bind(a.bind_parameter_count, "sqlite3_bind_parameter_count"); bind(a.bind_null, "sqlite3_bind_null");
    bind(a.bind_int64, "sqlite3_bind_int64"); bind(a.bind_double, "sqlite3_bind_double");
    bind(a.bind_text16, "sqlite3_bind_text16"); bind(a.bind_blob, "sqlite3_bind_blob");
    bind(a.column_count, "sqlite3_column_count"); bind(a.column_name16, "sqlite3_column_name16");
    bind(a.column_type, "sqlite3_column_type"); bind(a.column_int64, "sqlite3_column_int64");
    bind(a.column_double, "sqlite3_column_double"); bind(a.column_text16, "sqlite3_column_text16");
    bind(a.column_bytes16, "sqlite3_column_bytes16"); bind(a.column_blob, "sqlite3_column_blob");
    bind(a.column_bytes, "sqlite3_column_bytes"); bind(a.changes, "sqlite3_changes");
    bind(a.last_insert_rowid, "sqlite3_last_insert_rowid"); bind(a.extended_errcode, "sqlite3_extended_errcode");
    bind(a.errmsg16, "sqlite3_errmsg16"); bind(a.errstr, "sqlite3_errstr");
    a.cacheflush = reinterpret_cast<decltype(a.cacheflush)>(platform::sqlite_symbol("sqlite3_db_cacheflush"));
    return complete ? &a : nullptr;
  }();
  return loaded;
}

jclass native_class, long_class, double_class, integer_class, string_class, list_class, bytes_class, object_class;
jmethodID make_exception, long_value_of, double_value_of, integer_value_of, list_add, long_value, double_value;

jstring utf16(JNIEnv* env, const void* text, std::size_t units) {
  return env->NewString(static_cast<const jchar*>(text), static_cast<jsize>(units));
}
jstring utf16(JNIEnv* env, const void* text) {
  std::size_t units = 0;
  for (auto* p = static_cast<const char16_t*>(text); p[units]; ++units) {}
  return utf16(env, text, units);
}
jstring ascii(JNIEnv* env, const char* text) { return env->NewStringUTF(text); }

// AOSP throw_sqlite3_exception: SQLiteNative.exception picks the class.
void throw_sqlite(JNIEnv* env, int code, jstring message, jstring detail) {
  if (env->ExceptionCheck()) return;
  auto error = static_cast<jthrowable>(
      env->CallStaticObjectMethod(native_class, make_exception, static_cast<jint>(code), message, detail));
  if (!env->ExceptionCheck() && error) env->Throw(error);
  env->DeleteLocalRef(error);
  env->DeleteLocalRef(message);
  if (detail) env->DeleteLocalRef(detail);
}
void throw_sqlite(JNIEnv* env, sqlite3* db, jstring detail = nullptr) {
  throw_sqlite(env, api()->extended_errcode(db), utf16(env, api()->errmsg16(db)), detail);
}
jstring while_compiling(JNIEnv* env, jstring sql) {
  std::u16string text = u", while compiling: ";
  const auto base = text.size();
  const auto length = env->GetStringLength(sql);
  text.resize(base + static_cast<std::size_t>(length));
  env->GetStringRegion(sql, 0, length, reinterpret_cast<jchar*>(text.data() + base));
  return utf16(env, text.data(), text.size());
}
sqlite3* db_of(jlong handle) { return reinterpret_cast<sqlite3*>(static_cast<std::uintptr_t>(handle)); }
std::mutex databases_mutex;
std::set<sqlite3*> databases;

struct Statement {
  sqlite3_stmt* handle = nullptr;
  ~Statement() { if (handle) api()->finalize(handle); }
};
// Compiles the first statement of sql; the rest is ignored, as in AOSP.
bool prepare(JNIEnv* env, sqlite3* db, jstring sql, Statement& statement) {
  const auto length = env->GetStringLength(sql);
  const jchar* chars = env->GetStringChars(sql, nullptr);
  if (!chars) return false;
  const int result = api()->prepare16_v2(db, chars, length * 2, &statement.handle, nullptr);
  env->ReleaseStringChars(sql, chars);
  if (result != sqlite_ok) {
    throw_sqlite(env, db, while_compiling(env, sql));
    return false;
  }
  return true;
}
// Arguments are Long, Double, String, byte[] or null (SQLiteDatabase.bindValue).
bool bind(JNIEnv* env, sqlite3* db, sqlite3_stmt* statement, jobjectArray args) {
  const jsize count = args ? env->GetArrayLength(args) : 0;
  for (jsize i = 0; i < count; ++i) {
    jobject value = env->GetObjectArrayElement(args, i);
    const int index = i + 1;
    int result;
    if (!value) {
      result = api()->bind_null(statement, index);
    } else if (env->IsInstanceOf(value, long_class)) {
      result = api()->bind_int64(statement, index, env->CallLongMethod(value, long_value));
    } else if (env->IsInstanceOf(value, double_class)) {
      result = api()->bind_double(statement, index, env->CallDoubleMethod(value, double_value));
    } else if (env->IsInstanceOf(value, string_class)) {
      auto text = static_cast<jstring>(value);
      const jchar* chars = env->GetStringChars(text, nullptr);
      if (!chars) return false;
      result = api()->bind_text16(statement, index, chars, env->GetStringLength(text) * 2, transient);
      env->ReleaseStringChars(text, chars);
    } else if (env->IsInstanceOf(value, bytes_class)) {
      auto array = static_cast<jbyteArray>(value);
      std::vector<jbyte> bytes(static_cast<std::size_t>(env->GetArrayLength(array)));
      env->GetByteArrayRegion(array, 0, static_cast<jsize>(bytes.size()), bytes.data());
      static const jbyte empty = 0;  // a null pointer would bind NULL rather than an empty BLOB
      result = api()->bind_blob(statement, index, bytes.empty() ? &empty : bytes.data(),
                                static_cast<int>(bytes.size()), transient);
    } else {
      env->DeleteLocalRef(value);
      auto type = env->FindClass("java/lang/IllegalArgumentException");
      if (type) env->ThrowNew(type, "SQLite bind values are Long, Double, String, byte[] or null");
      return false;
    }
    env->DeleteLocalRef(value);
    if (env->ExceptionCheck()) return false;
    if (result != sqlite_ok) {
      throw_sqlite(env, db);
      return false;
    }
  }
  return true;
}
jobject column(JNIEnv* env, sqlite3_stmt* statement, int index) {
  switch (api()->column_type(statement, index)) {
    case type_integer:
      return env->CallStaticObjectMethod(long_class, long_value_of, static_cast<jlong>(api()->column_int64(statement, index)));
    case type_float:
      return env->CallStaticObjectMethod(double_class, double_value_of, api()->column_double(statement, index));
    case type_text: {
      const void* text = api()->column_text16(statement, index);
      return utf16(env, text, static_cast<std::size_t>(api()->column_bytes16(statement, index)) / 2);
    }
    case type_blob: {
      const void* data = api()->column_blob(statement, index);
      const int size = api()->column_bytes(statement, index);
      auto array = env->NewByteArray(size);
      if (array && size) env->SetByteArrayRegion(array, 0, size, static_cast<const jbyte*>(data));
      return array;
    }
    default:
      return nullptr;
  }
}

jlong JNICALL native_open(JNIEnv* env, jclass, jbyteArray path, jint flags) {
  if (!api()) {
    throw_sqlite(env, sqlite_cantopen, ascii(env, "system SQLite library is unavailable"),
          ascii(env, "Could not open database"));
    return 0;
  }
  std::vector<char> name(static_cast<std::size_t>(env->GetArrayLength(path)));
  env->GetByteArrayRegion(path, 0, static_cast<jsize>(name.size()), reinterpret_cast<jbyte*>(name.data()));
  sqlite3* db = nullptr;
  const int result = api()->open_v2(name.data(), &db, flags, nullptr);
  if (result != sqlite_ok) {
    if (db) {
      throw_sqlite(env, db, ascii(env, "Could not open database"));
      api()->close_v2(db);
    } else {
      throw_sqlite(env, result, ascii(env, api()->errstr(result)), ascii(env, "Could not open database"));
    }
    return 0;
  }
  api()->extended_result_codes(db, 1);
  api()->busy_timeout(db, busy_timeout_ms);
  if (os_quit::enabled) { std::lock_guard lock(databases_mutex); databases.insert(db); }
  return static_cast<jlong>(reinterpret_cast<std::uintptr_t>(db));
}
void JNICALL native_close(JNIEnv* env, jclass, jlong handle) {
  if (api()->close_v2(db_of(handle)) != sqlite_ok) throw_sqlite(env, db_of(handle), ascii(env, "Could not close database"));
  else if (os_quit::enabled) { std::lock_guard lock(databases_mutex); databases.erase(db_of(handle)); }
}
jobjectArray JNICALL native_prepare(JNIEnv* env, jclass, jlong handle, jstring sql) {
  auto* db = db_of(handle);
  Statement statement;
  if (!prepare(env, db, sql, statement)) return nullptr;
  const int parameters = statement.handle ? api()->bind_parameter_count(statement.handle) : 0;
  const int columns = statement.handle ? api()->column_count(statement.handle) : 0;
  auto names = env->NewObjectArray(columns, string_class, nullptr);
  for (int i = 0; names && i < columns; ++i) {
    auto name = utf16(env, api()->column_name16(statement.handle, i));
    env->SetObjectArrayElement(names, i, name);
    env->DeleteLocalRef(name);
  }
  if (!names) return nullptr;
  auto count = env->CallStaticObjectMethod(integer_class, integer_value_of, static_cast<jint>(parameters));
  if (env->ExceptionCheck()) return nullptr;
  auto result = env->NewObjectArray(2, object_class, nullptr);
  if (!result) return nullptr;
  env->SetObjectArrayElement(result, 0, count);
  env->SetObjectArrayElement(result, 1, names);
  env->DeleteLocalRef(count);
  env->DeleteLocalRef(names);
  return result;
}
jlong JNICALL native_execute(JNIEnv* env, jclass, jlong handle, jstring sql, jobjectArray args, jint mode) {
  auto* db = db_of(handle);
  Statement statement;
  if (!prepare(env, db, sql, statement)) return -1;
  if (!statement.handle) return mode == 0 ? 0 : -1;  // only whitespace or comments
  if (!bind(env, db, statement.handle, args)) return -1;
  const int result = api()->step(statement.handle);
  if (result == sqlite_row) {
    throw_sqlite(env, sqlite_ok, ascii(env, "unknown error"),
          ascii(env, "Queries can be performed using SQLiteDatabase query or rawQuery methods only."));
    return -1;
  }
  if (result != sqlite_done) {
    throw_sqlite(env, db);
    return -1;
  }
  if (mode == 1) return api()->changes(db);
  if (mode == 2) return api()->changes(db) > 0 ? api()->last_insert_rowid(db) : -1;
  return 0;
}
void JNICALL native_query(JNIEnv* env, jclass, jlong handle, jstring sql, jobjectArray args, jobject rows) {
  auto* db = db_of(handle);
  Statement statement;
  if (!prepare(env, db, sql, statement) || !statement.handle || !bind(env, db, statement.handle, args)) return;
  const int columns = api()->column_count(statement.handle);
  for (;;) {
    const int result = api()->step(statement.handle);
    if (result == sqlite_done) return;
    if (result != sqlite_row) {
      throw_sqlite(env, db);
      return;
    }
    auto row = env->NewObjectArray(columns, object_class, nullptr);
    if (!row) return;
    for (int i = 0; i < columns; ++i) {
      auto value = column(env, statement.handle, i);
      if (env->ExceptionCheck()) return;
      env->SetObjectArrayElement(row, i, value);
      if (value) env->DeleteLocalRef(value);
    }
    env->CallBooleanMethod(rows, list_add, row);
    env->DeleteLocalRef(row);
    if (env->ExceptionCheck()) return;
  }
}

jclass global_class(JNIEnv* env, const char* name) {
  auto local = env->FindClass(name);
  if (!local) return nullptr;
  auto global = static_cast<jclass>(env->NewGlobalRef(local));
  env->DeleteLocalRef(local);
  return global;
}
void register_natives(JNIEnv* env) {
  native_class = global_class(env, "android/database/sqlite/SQLiteNative");
  long_class = global_class(env, "java/lang/Long");
  double_class = global_class(env, "java/lang/Double");
  integer_class = global_class(env, "java/lang/Integer");
  string_class = global_class(env, "java/lang/String");
  list_class = global_class(env, "java/util/List");
  bytes_class = global_class(env, "[B");
  object_class = global_class(env, "java/lang/Object");
  if (env->ExceptionCheck()) return;
  make_exception = env->GetStaticMethodID(native_class, "exception",
      "(ILjava/lang/String;Ljava/lang/String;)Landroid/database/sqlite/SQLiteException;");
  long_value_of = env->GetStaticMethodID(long_class, "valueOf", "(J)Ljava/lang/Long;");
  double_value_of = env->GetStaticMethodID(double_class, "valueOf", "(D)Ljava/lang/Double;");
  integer_value_of = env->GetStaticMethodID(integer_class, "valueOf", "(I)Ljava/lang/Integer;");
  long_value = env->GetMethodID(long_class, "longValue", "()J");
  double_value = env->GetMethodID(double_class, "doubleValue", "()D");
  list_add = env->GetMethodID(list_class, "add", "(Ljava/lang/Object;)Z");
  if (env->ExceptionCheck()) return;
#define METHOD(name, signature, function) {const_cast<char*>(name), const_cast<char*>(signature), reinterpret_cast<void*>(function)}
  const JNINativeMethod methods[]{
      METHOD("open", "([BI)J", native_open), METHOD("close", "(J)V", native_close),
      METHOD("prepare", "(JLjava/lang/String;)[Ljava/lang/Object;", native_prepare),
      METHOD("execute", "(JLjava/lang/String;[Ljava/lang/Object;I)J", native_execute),
      METHOD("query", "(JLjava/lang/String;[Ljava/lang/Object;Ljava/util/List;)V", native_query)};
#undef METHOD
  env->RegisterNatives(native_class, methods, static_cast<jint>(std::size(methods)));
}
}  // namespace

void register_sqlite_natives() {
  static std::once_flag once;
  std::call_once(once, [] { add_framework_natives(register_natives); });
}
bool flush_sqlite_for_os_quit() {
  std::lock_guard lock(databases_mutex);
  if (databases.empty()) return true;
  const auto* native = api();
  if (!native || !native->cacheflush) return false;
  for (auto* database : databases) if (native->cacheflush(database) != sqlite_ok) return false;
  return true;
}
}  // namespace venture::java
