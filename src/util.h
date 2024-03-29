#define internal static

typedef uint8_t  u8;
typedef uint16_t u16;
typedef uint32_t u32;
typedef uint64_t u64;
typedef int8_t  i8;
typedef int16_t i16;
typedef int32_t i32;
typedef int64_t i64;
typedef i32 b32;
typedef float r32;
typedef double r64;

#define false 0
#define true 1
#define I8_MIN  -128
#define I8_MAX  127
#define U8_MAX  0xff
#define I16_MIN -0x8000
#define I16_MAX 0x7FFF
#define U16_MAX 0xFFFF
#define I32_MIN -0x80000000
#define I32_MAX 0x7FFFFFFF
#define U32_MAX 0xFFFFFFFF
#define R32_MIN -3.402823466e+38f
#define R32_MAX  3.402823466e+38f
#define I64_MIN -0x8000000000000000ULL
#define I64_MAX 0x7FFFFFFFFFFFFFFFULL

#define PI  3.141592653589793238462643f
#define TAU 6.283185307179586476925286f

#define array_count(x) (sizeof(x) / sizeof(x[0]))
#define for_count(i, c) for(u32 i = 0; i < (c); ++i)
#define min(x, y) ((x) < (y) ? (x) : (y))
#define max(x, y) ((x) > (y) ? (x) : (y))
#define clamp(a, b, x) (max((a), min((b), (x))))
#define absolute(x) ((x) >= 0 ? (x) : -(x))
#define bflip(x) (x = !x)
#define lerp(a, b, t) ((1 - (t)) * (a) + (t) * (b))

#if RELEASE
  #undef assert
  #define assert(x)
#else
  #ifndef assert
  #include <stdio.h>
  #define assert_(x) macrostring(x)
  #define assert(x) if(!(x)) { fprintf(stderr, "Assert (" __FILE__ ":" assert_(__LINE__) ":%s): " #x "\n", __func__); __builtin_trap(); }
  #endif
#endif

#define malloc_array(count, type) ((type*)malloc((count) * sizeof(type)))
#define malloc_array_zero(count, type) ((type*)zero_bytes((count) * sizeof(type), malloc((count) * sizeof(type))))
#define malloc_struct(type) malloc_array(1, type)

// Modulo, but positive.
internal i32 i32_wrap_upto(i32 x, i32 y)
{
  if(y == 0)
  {
    x = 0;
  }
  else
  {
    // TODO: Don't be retarded.
    while(x < 0) { x += y; }
    x = x % y;
  }

  return x;
}

typedef struct
{
  i32 x;
  i32 y;
} v2i;

internal b32 v2i_eq(v2i a, v2i b)
{
  return a.x == b.x && a.y == b.y;
}

typedef union
{
  struct
  {
    u8 x, y, z;
  };

  struct
  {
    u8 r, g, b;
  };
} v3u8;

internal b32 v3u8_eq(v3u8 a, v3u8 b)
{
  return a.x == b.x && a.y == b.y && a.z == b.z;
}

internal void* zero_bytes(u64 size, void* data)
{
  for(u64 i = 0;
      i < size;
      ++i)
  {
    ((u8*)data)[i] = 0;
  }

  return data;
}

internal void copy_bytes(u64 size, void* from, void* to)
{
  for(u64 i = 0;
      i < size;
      ++i)
  {
    ((u8*)to)[i] = ((u8*)from)[i];
  }
}

internal b32 bytes_eq(u64 size, void* a, void* b)
{
  b32 different = false;
  for(u64 i = 0;
      i < size && !different;
      ++i)
  {
    different |= (((u8*)a)[i] != ((u8*)b)[i]);
  }
  return !different;
}

#define zero(t) ((t){0})
#define zero_struct(x) zero_bytes(sizeof(x), &(x))
#define struct_eq(a, b) (bytes_eq(sizeof(a), &(a), &(b)))

#define bitset8_get(bitset, idx)  (((bitset)[(idx) >> 3] >> ((idx) & 7)) & 1)
#define bitset8_set(bitset, idx)   ((bitset)[(idx) >> 3] |= (1 << ((idx) & 7)))
#define bitset8_unset(bitset, idx) ((bitset)[(idx) >> 3] &= ~(1 << ((idx) & 7)))

#define BITSET32(name, size)  u32 name[(((size) + 31) >> 5)]
#define bitset32_get(bitset, idx)  (((bitset)[(idx) >> 5] >> ((idx) & 31)) & 1)
#define bitset32_set(bitset, idx)   ((bitset)[(idx) >> 5] |= (1 << ((idx) & 31)))
#define bitset32_unset(bitset, idx) ((bitset)[(idx) >> 5] &= ~(1 << ((idx) & 31)))

#define bitset64_get(bitset, idx)  (((bitset)[(idx) >> 6] >> ((idx) & 63)) & 1)
#define bitset64_set(bitset, idx)   ((bitset)[(idx) >> 6] |= (1ULL << ((idx) & 63)))
#define bitset64_unset(bitset, idx) ((bitset)[(idx) >> 6] &= ~(1ULL << ((idx) & 63)))
#define bitset64_set_atomic(bitset, idx) __sync_fetch_and_or(&(bitset)[(idx) >> 6], (1ULL << ((idx) & 63)))

internal b32 is_ascii(u8 c)
{
  return (c & 0x80) == 0;
}

internal b32 is_digit(u8 c)
{
  return c >= '0' && c <= '9';
}

internal b32 is_printable(u8 c)
{
  return c >= ' ' && c <= '~';
}

internal b32 is_upper(u8 c)
{
  return c >= 'A' && c <= 'Z';
}

internal b32 is_lower(u8 c)
{
  return c >= 'a' && c <= 'z';
}

internal b32 is_linebreak(u8 c)
{
  return c >= '\n' && c <= '\r';
}

internal b32 is_alpha(u8 c)
{
  return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z');
}

internal b32 is_utf8_continuation_byte(u8 c)
{
  return (c & 0xC0) == 0x80;
}

static const u32 replacement_character_codepoint = 0xFFFD;

internal u32 decode_utf8(u8** start, u8* end)
{
  u8 c = **start;
  u32 codepoint = 0;
  i32 extra_byte_count = 0;
  if(c & 0x80)
  {
    if((c & 0xE0) == 0xC0)
    {
      codepoint = (c & 0x1F);
      extra_byte_count = 1;
    }
    else if((c & 0xF0) == 0xE0)
    {
      codepoint = (c & 0x0F);
      extra_byte_count = 2;
    }
    else if((c & 0xF8) == 0xF0)
    {
      codepoint = (c & 0x07);
      extra_byte_count = 3;
    }
    else
    {
      // Invalid non-continuation byte.
      codepoint = replacement_character_codepoint;
    }

    if(*start + extra_byte_count < end)
    {
      b32 invalid_continuation = false;
      for(i32 extra_byte_idx = 0;
          extra_byte_idx < extra_byte_count;
          ++extra_byte_idx)
      {
        ++*start;
        c = **start;
        invalid_continuation = !is_utf8_continuation_byte(c);
        if(invalid_continuation)
        {
          // Expected continuation byte, but didn't get it.
          --*start;
          codepoint = replacement_character_codepoint;
          break;
        }
        codepoint = (codepoint << 6) | (c & 0x3F);
      }
    }
    else
    {
      // Expected more bytes.
      codepoint = replacement_character_codepoint;
    }
  }
  else
  {
    // ASCII.
    codepoint = (u32)c;
  }
  ++*start;

  return codepoint;
}

internal u8 to_lower(u8 c)
{
  u8 result = c;

  if(is_alpha(c))
  {
    result |= 0x20;
  }

  return result;
}

internal u8 to_upper(u8 c)
{
  u8 result = c;

  if(is_alpha(c))
  {
    result &= ~0x20;
  }

  return result;
}

typedef struct
{
  u8* data;
  size_t size;
} str_t;
#define str(x) ((str_t){ (u8*)(x), sizeof(x) - 1 })

#define STR_SIZE(x) ((x).size)

internal str_t wrap_str(char* z)
{
  str_t result = {0};
  result.data = (u8*)z;
  while(*z) { ++z; }
  result.size = (u8*)z - result.data;
  return result;
}

// For printf.
#define PF_STR(x) (int)((x).size), (x).data

internal str_t str_from_span(u8* start, u8* end)
{
  return (str_t){ start, end > start ? end - start : 0 };
}

internal str_t str_from_start_and_size(u8* start, size_t size)
{
  return (str_t){ start, size };
}

internal b32 str_eq(str_t a, str_t b)
{
  b32 result = (a.size == b.size);

  for(u64 i = 0; i < a.size && result; ++i)
  {
    if(a.data[i] != b.data[i])
    {
      result = false;
    }
  }

  return result;
}

internal b32 str_eq_ignoring_case(str_t a, str_t b)
{
  b32 result = (a.size == b.size);

  for(u64 i = 0;
      i < a.size && result;
      ++i)
  {
    if(to_lower(a.data[i]) != to_lower(b.data[i]))
    {
      result = false;
    }
  }

  return result;
}

internal int str_compare(str_t a, str_t b)
{
  int result = 0;

  for(u64 i = 0;
      i < a.size && i < b.size && result == 0;
      ++i)
  {
    if(a.data[i] < b.data[i]) { result = -1; }
    if(a.data[i] > b.data[i]) { result =  1; }
  }

  if(result == 0)
  {
    if(a.size < b.size) { result = -1; }
    if(a.size > b.size) { result =  1; }
  }

  return result;
}

internal b32 str_eq_zstr(str_t a, char* b)
{
  b32 result = true;

  for(u64 idx = 0;
      idx < a.size;
      ++idx)
  {
    if(b[idx] == 0 || a.data[idx] != b[idx])
    {
      result = false;
      break;
    }
  }

  if(result && b[a.size] != 0)
  {
    result = false;
  }

  return result;
}

internal b32 zstr_eq(char* a, char* b)
{
  b32 result = true;
  while(*a && *b && result)
  {
    result &= (*a++ == *b++);
  }
  result &= (*a++ == *b++);
  return result;
}

internal i32 zstr_length(const char* txt)
{
  i32 result = 0;

  if(txt)
  {
    while(*txt)
    {
      ++result;
      ++txt;
    }
  }

  return result;
}

internal b32 str_has_suffix(str_t input, str_t suffix)
{
  b32 result = false;

  if(input.size >= suffix.size)
  {
    str_t input_suffix = { input.data + input.size - suffix.size, suffix.size };
    if(str_eq(input_suffix, suffix))
    {
      result = true;
    }
  }

  return result;
}

internal str_t str_remove_suffix(str_t input, str_t suffix)
{
  if(str_has_suffix(input, suffix))
  {
    input.size -= suffix.size;
  }

  return input;
}

// If successful, string is zero-terminated (not included in size).
internal str_t read_file(char* path)
{
  str_t result = {0};
  FILE* fd = fopen(path, "rb");
  if(fd == 0)
  {
    fprintf(stderr, "Could not open file '%s' for reading.\n", path);
  }
  else
  {
    // This *must* be an int, not a long, otherwise the "-1" that ftell returns on error is wrong.
    int tell_pos = 0;

    if(fseek(fd, 0, SEEK_END) == -1 || (tell_pos = ftell(fd)) == -1)
    {
      fprintf(stderr, "Could not seek to end of file '%s'.\n", path);
    }
    else
    {
      result.size = tell_pos;
      fseek(fd, 0L, SEEK_SET);
      result.data = malloc_array(result.size + 1, u8);

      if(!result.data)
      {
        fprintf(stderr, "Could not allocate %lu bytes for file '%s'.\n", result.size, path);
        result.size = 0;
      }
      else if(fread(result.data, 1, result.size, fd) != result.size)
      {
        fprintf(stderr, "Could not read all of file '%s'.\n", path);
        free(result.data);
        result.data = 0;
        result.size = 0;
      }
      else
      {
        result.data[result.size] = 0;
      }
    }
  }

  if(fd)
  {
    fclose(fd);
  }

  return result;
}
