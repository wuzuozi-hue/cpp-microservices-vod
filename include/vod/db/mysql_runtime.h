#pragma once
#include <mysql/mysql.h>
#include <mutex>
#include <stdexcept>

namespace vod::db {
// mysql_library_init is process-wide and must finish before concurrent calls
// to mysql_init. The library remains initialized for the life of the process.
inline void ensure_mysql_runtime() {
    static std::once_flag once;
    std::call_once(once, [] { if (mysql_library_init(0, nullptr, nullptr)) throw std::runtime_error("MySQL runtime initialization failed"); });
}
}
