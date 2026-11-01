#include <cstddef>
#include <new>

// NOLINTBEGIN(misc-misplaced-const, readability-avoid-const-params-in-decls)
void *operator new[](size_t size, const char *, int, unsigned, const char *, int)
{
    return ::operator new[](size);
}

void *operator new[](size_t size, size_t alignment, size_t, const char *, int, unsigned, const char *, int)
{
    return ::operator new[](size, static_cast<std::align_val_t>(alignment));
}

// NOLINTEND(misc-misplaced-const, readability-avoid-const-params-in-decls)
