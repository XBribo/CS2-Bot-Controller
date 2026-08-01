// Resolves live server field offsets through SchemaSystem_001

#include "schema_resolver.h"

#include <schemasystem/schemasystem.h>

#if defined(_WIN32)
#include <Windows.h>
#else
#include <dlfcn.h>
#include <link.h>
#endif

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>

namespace BotController::Schema {
using CreateInterfaceFn = void* (*)(const char*, int*);

namespace {
ISchemaSystem* g_schemaSystem = nullptr;
CSchemaSystemTypeScope* g_serverScope = nullptr;
std::unordered_map<std::string, int> g_offsetCache;

#if defined(_WIN32)
constexpr const char* kSchemaModuleName = "schemasystem.dll";
constexpr const char* kServerScopeName = "server.dll";
#else
constexpr const char* kSchemaModuleName = "libschemasystem.so";
constexpr const char* kServerScopeName = "libserver.so";

struct FindModuleContext
{
    const char* moduleName = nullptr;
    const char* modulePath = nullptr;
};

// Returns the final component of a Linux module path
const char* BaseName(const char* path)
{
    if (!path) return "";
    const char* slash = std::strrchr(path, '/');
    return slash ? slash + 1 : path;
}

// Captures the full path of one already loaded Linux module
int FindModuleCallback(dl_phdr_info* info, size_t, void* data)
{
    auto* context = static_cast<FindModuleContext*>(data);
    if (info->dlpi_name && std::strcmp(BaseName(info->dlpi_name), context->moduleName) == 0)
    {
        context->modulePath = info->dlpi_name;
        return 1;
    }
    return 0;
}

// Opens an existing Linux module without loading a second copy
void* OpenLoadedModule(const char* moduleName)
{
    void* module = dlopen(moduleName, RTLD_NOW | RTLD_NOLOAD);
    if (module) return module;

    FindModuleContext context{};
    context.moduleName = moduleName;
    dl_iterate_phdr(FindModuleCallback, &context);
    return context.modulePath && context.modulePath[0] ? dlopen(context.modulePath, RTLD_NOW | RTLD_NOLOAD) : nullptr;
}
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
    Reset();

#if defined(_WIN32)
    HMODULE module = GetModuleHandleA(kSchemaModuleName);
    if (!module) return Fail(errorOut, errorOutLen, "schemasystem.dll is not loaded");
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(GetProcAddress(module, "CreateInterface"));
#else
    void* module = OpenLoadedModule(kSchemaModuleName);
    if (!module) return Fail(errorOut, errorOutLen, "libschemasystem.so is not loaded");
    auto createInterface = reinterpret_cast<CreateInterfaceFn>(dlsym(module, "CreateInterface"));
#endif

    if (!createInterface)
    {
#if !defined(_WIN32)
        dlclose(module);
#endif
        return Fail(errorOut, errorOutLen, "schemasystem CreateInterface export is unavailable");
    }

    g_schemaSystem = static_cast<ISchemaSystem*>(createInterface(SCHEMASYSTEM_INTERFACE_VERSION, nullptr));
#if !defined(_WIN32)
    dlclose(module);
#endif
    if (!g_schemaSystem) return Fail(errorOut, errorOutLen, "SchemaSystem_001 is unavailable");
    if (!g_schemaSystem->SchemaSystemIsReady())
    {
        Reset();
        return Fail(errorOut, errorOutLen, "SchemaSystem_001 is not ready");
    }

    g_serverScope = g_schemaSystem->FindTypeScopeForModule(kServerScopeName, nullptr);
    if (!g_serverScope)
    {
        Reset();
        return Fail(errorOut, errorOutLen, "server Schema type scope is unavailable");
    }
    return true;
}

// Returns one declared field offset, or -1 when it cannot be resolved
int GetFieldOffset(const char* className, const char* fieldName)
{
    if (!g_serverScope || !className || !fieldName) return -1;

    const std::string key = std::string(className) + "::" + fieldName;
    const auto cached = g_offsetCache.find(key);
    if (cached != g_offsetCache.end()) return cached->second;

    CSchemaClassInfo* classInfo = g_serverScope->FindDeclaredClass(className).Get();
    if (!classInfo || !classInfo->m_pFields)
    {
        g_offsetCache.emplace(key, -1);
        return -1;
    }

    for (uint16 i = 0; i < classInfo->m_nFieldCount; ++i)
    {
        const SchemaClassFieldData_t& field = classInfo->m_pFields[i];
        if (!field.m_pszName || std::strcmp(field.m_pszName, fieldName) != 0) continue;

        const int offset = field.m_nSingleInheritanceOffset;
        if (offset < 0 || offset >= classInfo->m_nSize) break;
        g_offsetCache.emplace(key, offset);
        return offset;
    }

    g_offsetCache.emplace(key, -1);
    return -1;
}

// Clears the cached interface, type scope, and field offsets
void Reset()
{
    g_offsetCache.clear();
    g_serverScope = nullptr;
    g_schemaSystem = nullptr;
}

} // namespace BotController::Schema
