// Resolves live server field offsets through SchemaSystem_001

#include "core/cs2_sdk/schema.h"

#include <schemasystem/schemasystem.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace cs2bc::schema {
namespace {
CSchemaSystemTypeScope* g_serverScope = nullptr;
using FieldMap = std::unordered_map<std::string, int>;
std::unordered_map<std::string, FieldMap> g_classCache; // NOLINT(bugprone-throwing-static-initialization)

#ifdef _WIN32
constexpr const char* kServerScopeName = "server.dll";
#else
constexpr const char* kServerScopeName = "libserver.so";
#endif

// Writes one resolver error into the caller-provided buffer
bool Fail(char* errorOut, size_t errorOutLen, const char* message)
{
    if (errorOut && errorOutLen > 0) std::snprintf(errorOut, errorOutLen, "%s", message);
    return false;
}
} // namespace

// Resolves the live SchemaSystem interface and server type scope
bool Init(char* errorOut, size_t errorOutLen)
{
    if (g_schemaSystem && g_serverScope) return true;

    g_classCache.clear();
    g_serverScope = nullptr;
    if (!g_schemaSystem) return Fail(errorOut, errorOutLen, "SchemaSystem interface is unavailable");
    if (!g_schemaSystem->SchemaSystemIsReady())
    {
        return Fail(errorOut, errorOutLen, "SchemaSystem_001 is not ready");
    }

    g_serverScope = g_schemaSystem->FindTypeScopeForModule(kServerScopeName, nullptr);
    if (!g_serverScope)
    {
        return Fail(errorOut, errorOutLen, "server Schema type scope is unavailable");
    }
    return true;
}

// Caches the declared fields of each class while validating their bounds.
int GetFieldOffset(const char* className, const char* fieldName)
{
    if (!g_serverScope || !className || !fieldName) return -1;
    auto table = g_classCache.find(className);
    if (table == g_classCache.end())
    {
        FieldMap fields;
        CSchemaClassInfo* info = g_serverScope->FindDeclaredClass(className).Get();
        if (info && info->m_pFields)
        {
            for (uint16_t i = 0; i < info->m_nFieldCount; ++i)
            {
                const auto& field = info->m_pFields[i];
                const int offset = field.m_nSingleInheritanceOffset;
                if (field.m_pszName && offset >= 0 && offset < info->m_nSize) fields.emplace(field.m_pszName, offset);
            }
        }
        table = g_classCache.emplace(className, std::move(fields)).first;
    }
    const auto field = table->second.find(fieldName);
    return field == table->second.end() ? -1 : field->second;
}

// Clears the cached interface, type scope, and field offsets
void Reset()
{
    g_classCache.clear();
    g_serverScope = nullptr;
}

} // namespace cs2bc::schema
