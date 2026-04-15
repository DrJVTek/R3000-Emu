#pragma once

namespace gte
{

enum class BackendKind
{
    faithful = 0,
    modern = 1,
};

inline const char* backend_kind_name(BackendKind kind)
{
    switch (kind)
    {
    case BackendKind::faithful: return "faithful";
    case BackendKind::modern: return "modern";
    default: return "faithful";
    }
}

} // namespace gte
