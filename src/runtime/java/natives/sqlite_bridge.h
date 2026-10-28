#pragma once
namespace venture::java {
// android.database.sqlite.SQLiteNative over the host's system SQLite. Call
// before start_jvm(); adds the registrar through jvm.h's existing contract.
void register_sqlite_natives();
bool flush_sqlite_for_os_quit(); // after Java/native execution is stopped
}  // namespace venture::java
