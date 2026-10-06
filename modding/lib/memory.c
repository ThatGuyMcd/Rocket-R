/* Freestanding MIPS helpers. Clang may emit these for structure copies even
 * with -fno-builtin. Weak definitions let a project supply its own version. */
typedef unsigned int Size;
__attribute__((weak)) void* memset(void* target, int value, Size length) {
    unsigned char* out = target;
    for (Size i = 0; i < length; ++i) out[i] = (unsigned char)value;
    return target;
}
__attribute__((weak)) void* memcpy(void* target, const void* source, Size length) {
    unsigned char* out = target; const unsigned char* in = source;
    for (Size i = 0; i < length; ++i) out[i] = in[i];
    return target;
}
__attribute__((weak)) void* memmove(void* target, const void* source, Size length) {
    unsigned char* out = target; const unsigned char* in = source;
    if (out < in) for (Size i = 0; i < length; ++i) out[i] = in[i];
    else while (length) { --length; out[length] = in[length]; }
    return target;
}
