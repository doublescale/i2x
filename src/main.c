// Defines sigaction on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <dirent.h>
#include <fcntl.h>
#include <inttypes.h>
#include <limits.h>
#include <locale.h>
#include <math.h>
#include <poll.h>
#include <pthread.h>
#include <semaphore.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/inotify.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>
#include <unistd.h>

// https://www.x.org/releases/current/doc/libX11/libX11/libX11.html
#include <X11/Xlib.h>
#include <X11/Xatom.h>
#define XK_MISCELLANY
#include <X11/keysymdef.h>
#undef XK_MISCELLANY

// https://fedoraproject.org/wiki/Features/XI2#Documentation
#include <X11/extensions/XInput2.h>

// https://www.khronos.org/registry/OpenGL/specs/gl/glx1.4.pdf
#define GL_GLEXT_PROTOTYPES
#define GLX_GLXEXT_PROTOTYPES
#include <GL/glx.h>
#undef GL_GLEXT_PROTOTYPES
#undef GLX_GLXEXT_PROTOTYPES
// #include <GL/glxext.h>
// #include <GL/glext.h>

#include "lib/stb_image.h"
#include "lib/stb_truetype.h"

#include "util.h"

#define WINDOW_INIT_W 800
#define WINDOW_INIT_H 600
#define PROGRAM_NAME "i2x"

static FILE* debug_out = 0;
#define DEBUG_LOG(...) if(debug_out) { fprintf(debug_out, __VA_ARGS__); }
// #define DEBUG_LOG(...)

// static void (*glXSwapIntervalEXT)(Display *dpy, GLXDrawable drawable, int interval);

internal u64 get_nanoseconds()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_nsec + 1000000000 * ts.tv_sec;
}

internal b32 is_directory(char* path)
{
  b32 result = false;
  int fd = open(path, O_DIRECTORY | O_PATH);
  if(fd != -1)
  {
    result = true;
    close(fd);
  }
  return result;
}

static u8 test_texels[] = {
  1, 1, 1, 255,
  255, 0, 0, 255,
  0, 255, 0, 255,
  0, 0, 255, 255,
  2, 2, 2, 255,
  255, 255, 0, 255,
  0, 255, 255, 255,
  255, 0, 255, 255,
  128, 128, 128, 255,
};
static i32 test_texture_w = 3;
static i32 test_texture_h = 3;

enum
{
  LOAD_STATE_UNLOADED = 0,
  LOAD_STATE_LOADING,
  LOAD_STATE_LOADED_INTO_RAM,
  LOAD_STATE_LOAD_FAILED,
};
typedef u32 load_state_t;

enum
{
  IMG_FLAG_MARKED = (1 << 0),
};
typedef u32 img_flags_t;

typedef struct img_entry_t
{
  str_t path;
  u64 timestamp;

  str_t file_header_data;
  str_t generation_parameters;
  str_t positive_prompt;
  str_t negative_prompt;
  str_t seed;
  str_t batch_size;
  str_t model;
  str_t sampler;
  str_t sampling_steps;
  str_t cfg;

  img_flags_t flags;
  i32 w;
  i32 h;
  u8* pixels;
  GLuint texture_id;
  u32 vram_bytes;

  struct img_entry_t* lru_prev;
  struct img_entry_t* lru_next;

  // This should stay at the end so zeroing the memory updates this last.
  volatile load_state_t load_state;
} img_entry_t;

typedef struct
{
  i32 win_w;
  i32 win_h;

  b32 vsync;
  i64 vram_bytes_used;
  b32 linear_sampling;
  b32 alpha_blend;
  i32 show_info;

  GLuint test_texture_id;

  GLuint font_texture_id;
  i32 chars_per_font_row;
  i32 chars_per_font_col;
  stbtt_fontinfo font;
  r32 stb_font_scale;
  r32 font_ascent;
  r32 font_descent;
  u8* font_texels;
  i32 font_texture_w;
  i32 font_texture_h;
  i32 font_char_w;
  i32 font_char_h;
  u32 fixed_codepoint_range_start;
  u32 fixed_codepoint_range_length;
  u32* custom_codepoints;
  i32 custom_codepoint_first_char_idx;
  i32 custom_codepoint_count;
  i32 next_custom_codepoint_idx;

  char** input_paths;
  i32 input_path_count;

  img_entry_t* img_entries;
  i32 total_img_capacity;
  i32 total_img_count;

  i32* filtered_img_idxs;
  i32 filtered_img_count;

  i32 viewing_filtered_img_idx;

  b32 searching;
  str_t search_str;
  i64 search_str_capacity;
  i32 img_idx_viewed_before_search;
  i64 selection_start;
  i64 selection_end;

  int inotify_fd;

  b32 zoom_from_original_size;  // If false, fit to window instead.

  b32 hide_sidebar;
  i32 sidebar_width;
  r32 sidebar_scroll_rows;
  i32 thumbnail_columns;

  i32 info_panel_width;

  r32 dragging_start_x;
  r32 dragging_start_y;
  i32 dragging_start_value;
  r32 dragging_start_value2;
  b32 mouse_moved_since_dragging_start;

  r32 xi_scroll_x_increment;
  r32 xi_scroll_y_increment;
  r32 xi_last_scroll_x_valuator;
  r32 xi_last_scroll_y_valuator;

  img_entry_t* lru_first;
  img_entry_t* lru_last;
} state_t;

internal void set_title(Display* display, Window window, u8* txt, i32 txt_len)
{
  XChangeProperty(display, window, XA_WM_NAME, XA_STRING, 8, PropModeReplace, txt, txt_len);
}

internal void xi_update_device_info(state_t* state, i32 class_count, XIAnyClassInfo** classes)
{
  // According to my tests, X sends device change events if the user starts using a different
  // input device (e.g. switching from touchpad to trackpoint on a laptop),
  // so tracking single global values for scrolling should be sufficient
  // instead of having to track them for every device separately.

  DEBUG_LOG("  Device info classes:\n");
  for(i32 class_idx = 0;
      class_idx < class_count;
      ++class_idx)
  {
    XIAnyClassInfo* class = classes[class_idx];
    // printf("    type: %d, sourceid: %d\n", class->type, class->sourceid);

    if(class->type == XIValuatorClass)
    {
      XIValuatorClassInfo* valuator_class = (XIValuatorClassInfo*)class;

      DEBUG_LOG("    ValuatorClass\n");
      DEBUG_LOG("      number: %d\n", valuator_class->number);
      DEBUG_LOG("      min: %f\n", valuator_class->min);
      DEBUG_LOG("      max: %f\n", valuator_class->max);
      DEBUG_LOG("      value: %f\n", valuator_class->value);

      if(valuator_class->number == 2)
      {
        state->xi_last_scroll_x_valuator = valuator_class->value;
      }
      if(valuator_class->number == 3)
      {
        state->xi_last_scroll_y_valuator = valuator_class->value;
      }
    }

    if(class->type == XIScrollClass)
    {
      XIScrollClassInfo* scroll_class = (XIScrollClassInfo*)class;

      DEBUG_LOG("    ScrollClass\n");
      DEBUG_LOG("      number: %d\n", scroll_class->number);
      DEBUG_LOG("      scroll_type: %d\n", scroll_class->scroll_type);
      DEBUG_LOG("      increment: %f\n", scroll_class->increment);
      DEBUG_LOG("      flags: %d\n", scroll_class->flags);

      if(scroll_class->number == 2)
      {
        state->xi_scroll_x_increment = scroll_class->increment;
      }
      if(scroll_class->number == 3)
      {
        state->xi_scroll_y_increment = scroll_class->increment;
      }
    }
  }
}

internal b32 advance_if_prefix_matches(u8** input, u8* input_end, char* prefix)
{
  b32 matches = true;
  u8* ptr = *input;

  while(*prefix)
  {
    if(ptr >= input_end || *ptr != *prefix)
    {
      matches = false;
      break;
    }

    ++prefix;
    ++ptr;
  }

  if(matches)
  {
    *input = ptr;
  }

  return matches;
}

internal str_t parse_next_json_str_destructively(u8** input, u8* input_end)
{
  u8* in = *input;
  while(in < input_end && *in != '"') { ++in; }
  if(in < input_end) { ++in; }

  u8* out = in;
  str_t result = {out};
  u16 utf16_high_surrogate = 0;
  while(in < input_end && *in != '"')
  {
    if(*in == '\\' && in + 1 < input_end)
    {
      ++in;
      if(0) {}
      else if(*in == 'b') { *out++ = '\b'; ++in; }
      else if(*in == 'f') { *out++ = '\f'; ++in; }
      else if(*in == 'n') { *out++ = '\n'; ++in; }
      else if(*in == 'r') { *out++ = '\r'; ++in; }
      else if(*in == 't') { *out++ = '\t'; ++in; }
      else if(*in == 'u' && in + 4 < input_end)
      {
        ++in;
        u16 utf16_code = 0;
        for_count(nibble_idx, 4)
        {
          utf16_code <<= 4;
          if(0) {}
          else if(*in >= '0' && *in <= '9') { utf16_code += *in - '0'; }
          else if(*in >= 'A' && *in <= 'F') { utf16_code += *in + 10 - 'A'; }
          else if(*in >= 'a' && *in <= 'f') { utf16_code += *in + 10 - 'a'; }
          ++in;
        }

        u32 c = (u32)utf16_code;
        if(utf16_code >= 0xD800 && utf16_code <= 0xDBFF)
        {
          // TODO: Emit the high surrogate if it's unpaired?
          //       Or also swallow unpaired high surrogates?
          //       Either way, don't match surrogate pairs across other things.
          utf16_high_surrogate = utf16_code;
        }
        else if(utf16_high_surrogate && utf16_code >= 0xDC00 && utf16_code <= 0xDFFF)
        {
          c = 0x10000 + ((u32)(utf16_high_surrogate - 0xD800) << 10) + (u32)(utf16_code - 0xDC00);
          utf16_high_surrogate = 0;
        }
        else
        {
          utf16_high_surrogate = 0;
        }

        if(!utf16_high_surrogate)
        {
          // UTF-32 to UTF-8.
          i32 utf8_bytes = 1;
          if(c >= 0x00080) { utf8_bytes = 2; }
          if(c >= 0x00800) { utf8_bytes = 3; }
          if(c >= 0x10000) { utf8_bytes = 4; }
          if(out + utf8_bytes <= input_end)
          {
            if(utf8_bytes == 1)
            {
              *out++ = c;
            }
            if(utf8_bytes == 2)
            {
              *out++ = 0xC0 | (c >> 6);
              *out++ = 0x80 | (c & 0x3F);
            }
            if(utf8_bytes == 3)
            {
              *out++ = 0xE0 | (c >> 12);
              *out++ = 0x80 | ((c >> 6) & 0x3F);
              *out++ = 0x80 | (c & 0x3F);
            }
            if(utf8_bytes == 4)
            {
              *out++ = 0xF0 | (c >> 18);
              *out++ = 0x80 | ((c >> 12) & 0x3F);
              *out++ = 0x80 | ((c >> 6) & 0x3F);
              *out++ = 0x80 | (c & 0x3F);
            }
          }
        }
      }
      else { *out++ = *in++; }
    }
    else
    {
      *out++ = *in++;
    }
  }

  result.size = out - result.data;
  *input = in;
  return result;
}

internal b32 is_seeking_word_separator(u8 c)
{
  return 0
    || c == ' '
    || c == '\n'
    || c == ':'
    || c == '/'
    || c == '|'
    ;
}

internal b32 is_linewrap_word_separator(u8 c)
{
  return 0
    || c == '-'
    || c == ':'
    || c == '/'
    || c == '|'
    ;
}

internal i64 seek_left_in_str(str_t str, b32 word_wise, i64 start_idx)
{
  i64 result = start_idx;

  while(word_wise && result > 0 && is_seeking_word_separator(str.data[result - 1]))
  {
    --result;
  }
  if(result > 0)
  {
    --result;
  }
  while(result > 0 &&
      (is_utf8_continuation_byte(str.data[result]) ||
       (word_wise && !is_seeking_word_separator(str.data[result - 1]))))
  {
    --result;
  }

  return result;
}

internal i64 seek_right_in_str(str_t str, b32 word_wise, i64 start_idx)
{
  i64 result = start_idx;

  while(word_wise && result < str.size && is_seeking_word_separator(str.data[result]))
  {
    ++result;
  }
  if(result < str.size)
  {
    ++result;
  }
  while(result < str.size &&
      (is_utf8_continuation_byte(str.data[result]) ||
       (word_wise && !is_seeking_word_separator(str.data[result]))))
  {
    ++result;
  }

  return result;
}

internal b32 str_replace_selection(i64 str_capacity, str_t* str,
    i64* selection_start, i64* selection_end, str_t new_contents)
{
  b32 result = false;

  i64 selection_min = min(*selection_start, *selection_end);
  i64 selection_max = max(*selection_start, *selection_end);
  i64 selection_length = selection_max - selection_min;
  i64 str_length_change = new_contents.size - selection_length;

  if(str->size + str_length_change <= str_capacity)
  {
    if(str_length_change > 0)
    {
      // Move characters after the insertion to the right.
      for(i64 move_idx = str->size + str_length_change - 1;
          move_idx >= selection_max + str_length_change;
          --move_idx)
      {
        str->data[move_idx] = str->data[move_idx - str_length_change];
      }
    }

    // Insert new characters.
    for(i64 new_idx = 0;
        new_idx < new_contents.size;
        ++new_idx)
    {
      str->data[selection_min + new_idx] = new_contents.data[new_idx];
    }

    if(str_length_change < 0)
    {
      // Move characters after the insertion to the left.
      i64 length_reduction = -str_length_change;
      for(i64 move_idx = selection_max - length_reduction;
          move_idx < str->size - length_reduction;
          ++move_idx)
      {
        str->data[move_idx] = str->data[move_idx + length_reduction];
      }
    }

    str->size += str_length_change;
    *selection_end = selection_min + new_contents.size;
    *selection_start = *selection_end;

    result = true;
  }

  return result;
}

typedef struct
{
  i32 total_img_count;
  img_entry_t* img_entries;

  i32 filtered_img_count;
  i32* filtered_img_idxs;

  volatile i32 viewing_filtered_img_idx;
  volatile i32 first_visible_thumbnail_idx;
  volatile i32 last_visible_thumbnail_idx;
} shared_loader_data_t;

typedef struct
{
  i32 thread_idx;
  sem_t* semaphore;
  shared_loader_data_t* shared;
} loader_data_t;

internal void* loader_fun(void* raw_data)
{
  loader_data_t* data = (loader_data_t*)raw_data;
  i32 thread_idx = data->thread_idx;
  shared_loader_data_t* shared = data->shared;

  for(;;)
  {
    i32 viewing_filtered_img_idx = shared->viewing_filtered_img_idx;
    i32 range_start_idx = shared->first_visible_thumbnail_idx;
    i32 range_end_idx = shared->last_visible_thumbnail_idx;
    i32 filtered_img_count = shared->filtered_img_count;

    // printf("Loader %d focus %d range_start %d range_end %d\n", thread_idx, viewing_filtered_img_idx, range_start_idx, range_end_idx);

    // i32 max_loading_idx = min(filtered_img_count + 1, 800);
    i32 max_loading_idx = min(filtered_img_count + 1, 200);
    for(i32 loading_idx = 0;
        loading_idx < max_loading_idx;
        ++loading_idx)
    {
      if(0
          || viewing_filtered_img_idx != shared->viewing_filtered_img_idx
          || range_start_idx != shared->first_visible_thumbnail_idx
          || range_end_idx != shared->last_visible_thumbnail_idx
          || filtered_img_count != shared->filtered_img_count
          || shared->filtered_img_count == 0
        )
      {
        break;
      }

      // Load the viewed image, then the thumbnail range, then spiral around the thumbnail range.
      i32 filtered_img_idx = 0;
      if(loading_idx == 0)
      {
        filtered_img_idx = viewing_filtered_img_idx;
      }
      else
      {
        filtered_img_idx = range_start_idx + loading_idx - 1;
        i32 extra_range_idx = filtered_img_idx - range_end_idx;
        if(extra_range_idx > 0)
        {
          if(extra_range_idx % 2 == 0)
          {
            filtered_img_idx = range_start_idx - (extra_range_idx + 1) / 2;
          }
          else
          {
            filtered_img_idx = range_end_idx + (extra_range_idx + 1) / 2;
          }
        }
        filtered_img_idx = i32_wrap_upto(filtered_img_idx, shared->filtered_img_count);
      }

      i32 img_idx = shared->filtered_img_idxs[filtered_img_idx];

      // printf("Loader %d loading_idx %d [%5d/%d]\n", thread_idx, loading_idx, img_idx + 1, shared->total_img_count);

      img_entry_t* img = &shared->img_entries[img_idx];

      if(__sync_bool_compare_and_swap(&img->load_state, LOAD_STATE_UNLOADED, LOAD_STATE_LOADING))
      {
#if 0
        printf("Loader %d [%5d/%d] %s\n",
            thread_idx,
            img_idx + 1, shared->total_img_count,
            img->path.data);
#endif

        struct statx file_stats = {0};
        u64 timestamp = 0;
        if(statx(AT_FDCWD, (char*)img->path.data, AT_STATX_SYNC_AS_STAT, STATX_MTIME | STATX_SIZE, &file_stats) != -1)
        {
          // printf("\nFile %.*s:\n", (int)img->path.size, img->path.data);
          // printf("  size: %llu\n", file_stats.stx_size);
          // printf("  modify time: %lli %u\n", file_stats.stx_mtime.tv_sec, file_stats.stx_mtime.tv_nsec);
          timestamp = (((u64)file_stats.stx_mtime.tv_sec << 32) | (u64)file_stats.stx_mtime.tv_nsec);
        }

        if(img->timestamp != timestamp)
        {
          img->timestamp = timestamp;
#if 1
          if(img->pixels)
          {
            free(img->pixels);
            img->pixels = 0;
            img->w = 0;
            img->h = 0;
          }
#endif
        }

        // TODO: Maybe put the resulting heap-allocated pointers onto a rolling "output" buffer
        //       so the main thread can deal with putting those into the actual image entries,
        //       to avoid race conditions with directory updates.
        if(!img->pixels)
        {
          i32 original_channel_count = 0;
          img->pixels = stbi_load((char*)img->path.data, &img->w, &img->h, &original_channel_count, 4);

          if(!img->pixels)
          {
            img->load_state = LOAD_STATE_LOAD_FAILED;
          }
          else
          {
#if 1
            // printf("Channels: %d\n", original_channel_count);
            if(original_channel_count == 4)
            {
              // Premultiply alpha.
              // printf("Premultiplying alpha.\n");
              for(u64 i = 0; i < (u64)img->w * (u64)img->h; ++i)
              {
                if(img->pixels[4*i + 3] != 255)
                {
                  r32 r = img->pixels[4*i + 0] / 255.0f;
                  r32 g = img->pixels[4*i + 1] / 255.0f;
                  r32 b = img->pixels[4*i + 2] / 255.0f;
                  r32 a = img->pixels[4*i + 3] / 255.0f;

                  img->pixels[4*i + 0] = (u8)(255.0f * a * r + 0.5f);
                  img->pixels[4*i + 1] = (u8)(255.0f * a * g + 0.5f);
                  img->pixels[4*i + 2] = (u8)(255.0f * a * b + 0.5f);
                }
              }
            }
#endif

            img->load_state = LOAD_STATE_LOADED_INTO_RAM;
          }
        }
        else
        {
          img->load_state = LOAD_STATE_LOADED_INTO_RAM;
        }
      }
    }

    // printf("Loader %d: Waiting on semaphore.\n", thread_idx);
    sem_wait(data->semaphore);
    // printf("Loader %d: Got signal!\n", thread_idx);
  }

  return 0;
}

internal void unload_texture(state_t* state, img_entry_t* unload)
{
  if(unload->lru_prev)
  {
    unload->lru_prev->lru_next = unload->lru_next;
  }
  else
  {
    state->lru_first = unload->lru_next;
  }

  if(unload->lru_next)
  {
    unload->lru_next->lru_prev = unload->lru_prev;
  }
  else
  {
    state->lru_last = unload->lru_prev;
  }

  // printf("Unloading texture ID %u\n", unload->texture_id);

  glDeleteTextures(1, &unload->texture_id);
  state->vram_bytes_used -= unload->vram_bytes;
  unload->vram_bytes = 0;
  unload->texture_id = 0;
  unload->lru_prev = 0;
  unload->lru_next = 0;

  stbi_image_free(unload->pixels);
  unload->w = 0;
  unload->h = 0;
  unload->pixels = 0;
  unload->load_state = LOAD_STATE_UNLOADED;
}

internal b32 upload_img_texture(state_t* state, img_entry_t* img)
{
  u64 nsecs_start = get_nanoseconds();
  i32 num_deletions = 0;
  i32 num_uploads = 0;

  b32 result = false;

  if(!img->texture_id)
  {
    if(img->load_state == LOAD_STATE_LOADED_INTO_RAM)
    {
      i64 texture_bytes = 4 * img->w * img->h;

      i64 vram_byte_limit = 1 * 1024 * 1024 * 1024;
      while(state->vram_bytes_used + texture_bytes > vram_byte_limit)
      {
        img_entry_t* unload = state->lru_last;
        if(unload)
        {
          ++num_deletions;
          unload_texture(state, unload);
        }
      }

      glGenTextures(1, &img->texture_id);

      glBindTexture(GL_TEXTURE_2D, img->texture_id);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
      if(state->linear_sampling)
      {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
      }
      else
      {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
          img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->pixels);
      glGenerateMipmap(GL_TEXTURE_2D);
      ++num_uploads;

      img->vram_bytes = texture_bytes;
      state->vram_bytes_used += texture_bytes;
      GLint tmp = 0;
      // printf("VRAM used: approx. %.0f MiB\n", (r64)state->vram_bytes_used / (1024.0 * 1024.0));

      // glGetIntegerv(GL_GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX, &tmp);
      // printf("  GPU_MEMORY_INFO_DEDICATED_VIDMEM_NVX:         %d kiB\n", tmp);

      // glGetIntegerv(GL_GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX, &tmp);
      // printf("  GPU_MEMORY_INFO_TOTAL_AVAILABLE_MEMORY_NVX:   %d kiB\n", tmp);

      // glGetIntegerv(GL_GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX, &tmp);
      // printf("  GPU_MEMORY_INFO_CURRENT_AVAILABLE_VIDMEM_NVX: %d kiB\n", tmp);

      // glGetIntegerv(GL_GPU_MEMORY_INFO_EVICTION_COUNT_NVX, &tmp);
      // printf("  GPU_MEMORY_INFO_EVICTION_COUNT_NVX: %d\n", tmp);

      // glGetIntegerv(GL_GPU_MEMORY_INFO_EVICTED_MEMORY_NVX, &tmp);
      // printf("  GPU_MEMORY_INFO_EVICTED_MEMORY_NVX: %d kiB\n", tmp);
    }
    else if(img->load_state != LOAD_STATE_LOAD_FAILED)
    {
      result = true;
    }
  }

  if(img->texture_id)
  {
    if(!state->lru_first)
    {
      state->lru_first = img;
      state->lru_last = img;
    }
    else
    {
      if(img != state->lru_first)
      {
        if(img == state->lru_last)
        {
          state->lru_last = img->lru_prev;
        }

        if(img->lru_next)
        {
          img->lru_next->lru_prev = img->lru_prev;
        }
        if(img->lru_prev)
        {
          img->lru_prev->lru_next = img->lru_next;
        }

        state->lru_first->lru_prev = img;
        img->lru_next = state->lru_first;
        img->lru_prev = 0;
        state->lru_first = img;
      }
    }
  }

#if 0
  {
    printf("LRU chain:");
    img_entry_t* detected_last = 0;
    for(img_entry_t* i = lru_first; i; i = i->lru_next)
    {
      printf(" %u", i->texture_id);
      detected_last = i;
    }
    if(state->lru_last != detected_last) { printf(", WRONG last: %u", state->lru_last->texture_id); }
    printf("\n");
  }
#endif

#if 0
  u64 nsecs_end = get_nanoseconds();
  u64 nsecs_taken = nsecs_end - nsecs_start;
  if(nsecs_taken > 1 * 1000 * 1000)
  {
    printf("upload_img_texture took %.0f ms (%d deletions, %d uploads)\n",
        1e-6 * (r64)nsecs_taken,
        num_deletions,
        num_uploads);
  }
#endif

  return result;
}

typedef struct
{
  void* ptr;
} ui_interaction_t;

internal b32 interaction_is_empty(ui_interaction_t a)
{
  return a.ptr == 0;
}

internal b32 interaction_eq(ui_interaction_t a, ui_interaction_t b)
{
  return a.ptr == b.ptr;
}

internal b32 interaction_allowed(ui_interaction_t current, ui_interaction_t target)
{
  return current.ptr == 0 || current.ptr == target.ptr;
}

internal i32 get_scrollbar_width(state_t* state)
{
  return 20;
}

internal i32 get_effective_sidebar_width(state_t* state)
{
  return max(0, min(state->win_w - 10, state->sidebar_width));
}

internal r32 get_thumbnail_size(state_t* state)
{
  return max(1.0f, (r32)(get_effective_sidebar_width(state) - get_scrollbar_width(state) - 2) / (r32)state->thumbnail_columns);
}

internal void clamp_sidebar_scroll_rows(state_t* state)
{
  r32 thumbnail_h = get_thumbnail_size(state);
  if(thumbnail_h > 0)
  {
    i32 thumbnail_rows = (state->filtered_img_count + state->thumbnail_columns - 1) / state->thumbnail_columns;

    state->sidebar_scroll_rows = max(0,
        min(thumbnail_rows - state->win_h / thumbnail_h + 1,
          state->sidebar_scroll_rows));
  }
}

internal void clamp_thumbnail_columns(state_t* state)
{
  state->thumbnail_columns = clamp(1, 64, state->thumbnail_columns);
}

internal img_entry_t* get_filtered_img(state_t* state, i32 filtered_idx)
{
  img_entry_t* result = 0;

  if(filtered_idx >= 0 && filtered_idx < state->filtered_img_count)
  {
    result = &state->img_entries[state->filtered_img_idxs[filtered_idx]];
  }

  return result;
}

internal void set_or_unset_filtered_img_flag(state_t* state, i32 filtered_idx, img_flags_t flags, b32 set)
{
  img_entry_t* img = get_filtered_img(state, filtered_idx);
  if(img)
  {
    if(set)
    {
      img->flags |= flags;
    }
    else
    {
      img->flags &= ~flags;
    }
  }
}

internal int compare_paths(const void* void_a, const void* void_b, void* void_data)
{
  return strcmp(*(char**)void_a, *(char**)void_b);
}

internal void reload_input_paths(state_t* state)
{
  i32 prev_viewing_idx = state->filtered_img_idxs[state->viewing_filtered_img_idx];
  str_t prev_viewing_path = state->img_entries[prev_viewing_idx].path;

  state->filtered_img_count = 0;
  state->total_img_count = 0;

  for(i32 input_path_idx = 0;
      input_path_idx < state->input_path_count;
      ++input_path_idx)
  {
    char* arg = state->input_paths[input_path_idx];
    b32 is_a_dir = false;
    DIR* dir = opendir(arg);

    if(dir)
    {
      struct dirent* dirent;
      i32 paths_in_dir_capacity = 1024 * 1024;
      i32 paths_in_dir_count = 0;
      char** paths_in_dir = malloc_array(paths_in_dir_capacity, char*);
      while((dirent = readdir(dir)) && paths_in_dir_count < paths_in_dir_capacity)
      {
        is_a_dir = true;
        char* path = dirent->d_name;

        // if(!zstr_eq(path, ".") && !zstr_eq(path, ".."))
        if(path[0] != '.')
        {
          char* full_path = 0;
          if(asprintf(&full_path, "%s/%s", arg, path) != -1)
          {
            paths_in_dir[paths_in_dir_count++] = full_path;
          }
        }
      }

      qsort_r(paths_in_dir, paths_in_dir_count, sizeof(paths_in_dir[0]), compare_paths, 0);

      for(i32 path_idx = 0;
          path_idx < paths_in_dir_count;
          ++path_idx)
      {
        i32 img_idx = state->total_img_count++;
        img_entry_t* img = &state->img_entries[img_idx];
        // TODO: Possibly free the previous path string.
        img->path = wrap_str(paths_in_dir[path_idx]);
        if(img->texture_id)
        {
          unload_texture(state, img);
        }
        img->load_state = LOAD_STATE_UNLOADED;

        if(str_eq(img->path, prev_viewing_path))
        {
          state->viewing_filtered_img_idx = img_idx;
        }
      }

      free(paths_in_dir);
      closedir(dir);
    }

    if(!is_a_dir)
    {
      i32 img_idx = state->total_img_count++;
      img_entry_t* img = &state->img_entries[img_idx];
      img->path = wrap_str(arg);
      if(img->texture_id)
      {
        unload_texture(state, img);
      }
      img->load_state = LOAD_STATE_UNLOADED;

      if(str_eq(img->path, prev_viewing_path))
      {
        state->viewing_filtered_img_idx = img_idx;
      }
    }

    if(state->inotify_fd != -1)
    {
      int watch_fd = inotify_add_watch(state->inotify_fd, arg,
          IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF /* | IN_MODIFY */ | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO
          | IN_EXCL_UNLINK /* | IN_ONLYDIR */);
    }
  }

  for(i32 img_idx = 0;
      img_idx < state->total_img_count;
      ++img_idx)
  {
    img_entry_t* img = &state->img_entries[img_idx];

    if(img->file_header_data.size)
    {
      free(img->file_header_data.data);
      zero_struct(img->file_header_data);
    }

    int fd = open((char*)img->path.data, O_RDONLY);
    if(fd == -1) { continue; }
    size_t bytes_to_read = 4 * 1024;
    img->file_header_data.data = malloc_array(bytes_to_read, u8);
    ssize_t bytes_actually_read = read(fd, img->file_header_data.data, bytes_to_read);
    close(fd);
    if(bytes_actually_read == -1) { continue; }
    img->file_header_data.size = bytes_actually_read;

    // Look for PNG metadata.
    if(img->file_header_data.size >= 16)
    {
      u8* ptr = img->file_header_data.data;
      u8* data_end = img->file_header_data.data + img->file_header_data.size;
      b32 bad = false;

      // https://www.w3.org/TR/2003/REC-PNG-20031110/#5PNG-file-signature
      bad |= (*ptr++ != 0x89);
      bad |= (*ptr++ != 'P');
      bad |= (*ptr++ != 'N');
      bad |= (*ptr++ != 'G');
      bad |= (*ptr++ != 0x0d);
      bad |= (*ptr++ != 0x0a);
      bad |= (*ptr++ != 0x1a);
      bad |= (*ptr++ != 0x0a);

      while(!bad)
      {
        // Convert big-endian to little-endian.
        u32 chunk_size = 0;
        chunk_size |= *ptr++;
        chunk_size <<= 8;
        chunk_size |= *ptr++;
        chunk_size <<= 8;
        chunk_size |= *ptr++;
        chunk_size <<= 8;
        chunk_size |= *ptr++;

        // https://www.w3.org/TR/2003/REC-PNG-20031110/#11tEXt
        b32 tEXt_header = (ptr[0] == 't' && ptr[1] == 'E' && ptr[2] == 'X' && ptr[3] == 't');
        b32 iTXt_header = (ptr[0] == 'i' && ptr[1] == 'T' && ptr[2] == 'X' && ptr[3] == 't');
        if(tEXt_header || iTXt_header)
        {
          u8* key_start = ptr + 4;
          u8* value_end = ptr + 4 + chunk_size;

          if(value_end <= data_end)
          {
            i32 key_len = 0;
            while(key_start[key_len] != 0 && key_start + key_len + 1 < value_end)
            {
              ++key_len;
            }

            u8* value_start = key_start + key_len + 1;
            // https://www.w3.org/TR/2003/REC-PNG-20031110/#11iTXt
            if(iTXt_header)
            {
              value_start += 2;
              while(*value_start) { ++value_start; }
              ++value_start;
              while(*value_start) { ++value_start; }
              ++value_start;
            }

            str_t key = { key_start, key_len };
            str_t value = str_from_span(value_start, value_end);
            // printf("tEXt: %.*s: %.*s\n", (int)key.size, key.data, (int)value.size, value.data);

            if(str_eq_zstr(key, "prompt"))
            {
              // comfyanonymous/ComfyUI JSON.
              // TODO: Tokenize properly, or string contents might get mistaken for object keys,
              //       like {"seed": "tricky \"seed:"}

              for(u8* p = value_start;
                  p < value_end;
                 )
              {
                if(0) {}
                else if(advance_if_prefix_matches(&p, value_end, "\"seed\"")
                    || advance_if_prefix_matches(&p, value_end, "\"noise_seed\""))
                {
                  while(p < value_end && !is_digit(*p)) { ++p; }
                  str_t v = {p};
                  while(p < value_end && is_digit(*p)) { ++p; }
                  v.size = p - v.data;

                  img->seed = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "\"steps\""))
                {
                  while(p < value_end && !is_digit(*p)) { ++p; }
                  str_t v = {p};
                  while(p < value_end && is_digit(*p)) { ++p; }
                  v.size = p - v.data;

                  img->sampling_steps = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "\"cfg\""))
                {
                  while(p < value_end && !(is_digit(*p) || *p == '.')) { ++p; }
                  str_t v = {p};
                  while(p < value_end && (is_digit(*p) || *p == '.')) { ++p; }
                  v.size = p - v.data;

                  img->cfg = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "\"sampler_name\""))
                {
                  str_t v = parse_next_json_str_destructively(&p, value_end);
                  img->sampler = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "\"ckpt_name\""))
                {
                  str_t v = parse_next_json_str_destructively(&p, value_end);
                  v = str_remove_suffix(v, str(".ckpt"));
                  v = str_remove_suffix(v, str(".safetensors"));
                  img->model = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "\"batch_size\""))
                {
                  while(p < value_end && !is_digit(*p)) { ++p; }
                  str_t v = {p};
                  while(p < value_end && is_digit(*p)) { ++p; }
                  v.size = p - v.data;

                  img->batch_size = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "\"text\""))
                {
                  str_t v = parse_next_json_str_destructively(&p, value_end);
                  if(!img->positive_prompt.data) { img->positive_prompt = v; }
                  else if(!img->negative_prompt.data) { img->negative_prompt = v; }
                }
                else
                {
                  ++p;
                }
              }
            }
            else if(str_eq_zstr(key, "parameters"))
            {
              // AUTOMATIC1111/stable-diffusion-webui.
              // Be careful with newlines and misleading labels in the
              // positive and negative prompts; use the last found keywords.

              u8* p = value_start;
              u8* negative_prompt_label_start = 0;
              u8* steps_label_start = value_end;

              while(p < value_end)
              {
                u8* p_prev = p;
                if(0) {}
                else if(advance_if_prefix_matches(&p, value_end, "\nNegative prompt: "))
                {
                  negative_prompt_label_start = p_prev;
                  img->negative_prompt.data = p;
                }
                else if(advance_if_prefix_matches(&p, value_end, "\nSteps: "))
                {
                  steps_label_start = p_prev;
                  img->sampling_steps.data = p;
                }

                ++p;
              }

              if(negative_prompt_label_start)
              {
                img->positive_prompt = str_from_span(value_start, negative_prompt_label_start);
              }
              else
              {
                img->positive_prompt = str_from_span(value_start, steps_label_start);
              }

              if(img->negative_prompt.data)
              {
                img->negative_prompt.size = steps_label_start - img->negative_prompt.data;
              }

              p = steps_label_start;

              while(p < value_end)
              {
                u8* p_prev = p;
                if(0) {}
                else if(advance_if_prefix_matches(&p, value_end, "Steps: "))
                {
                  str_t v = {p};
                  while(p < value_end && *p != ',') { ++p; }
                  v.size = p - v.data;

                  img->sampling_steps = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "Sampler: "))
                {
                  str_t v = {p};
                  while(p < value_end && *p != ',') { ++p; }
                  v.size = p - v.data;

                  img->sampler = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "CFG scale: "))
                {
                  str_t v = {p};
                  while(p < value_end && *p != ',') { ++p; }
                  v.size = p - v.data;

                  img->cfg = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "Seed: "))
                {
                  str_t v = {p};
                  while(p < value_end && *p != ',') { ++p; }
                  v.size = p - v.data;

                  img->seed = v;
                }
                else if(advance_if_prefix_matches(&p, value_end, "Model: "))
                {
                  str_t v = {p};
                  while(p < value_end && *p != ',') { ++p; }
                  v.size = p - v.data;

                  img->model = v;
                }

                ++p;
              }
            }

            if(!img->generation_parameters.size)
            {
              img->generation_parameters = value;
            }
          }
          else
          {
            bad = true;
          }
        }

        ptr += 4 + chunk_size + 4;

        bad |= (ptr + 8 >= data_end);
      }
    }

#if 0
    printf("\n%.*s\n", PF_STR(img->path));
#define P(x) printf("  " #x ": %.*s\n", (int)img->x.size, img->x.data);
    // P(generation_parameters);
    P(positive_prompt);
    P(negative_prompt);
    P(seed);
    P(batch_size);
    P(model);
    P(sampler);
    P(sampling_steps);
    P(cfg);
#undef P
#endif
  }

  for(i32 idx = 0;
      idx < state->total_img_count;
      ++idx)
  {
    state->filtered_img_idxs[idx] = idx;
  }
  state->filtered_img_count = state->total_img_count;
}

enum
{
  DRAW_STR_MEASURE_ONLY = (1 << 0),
};
typedef u32 draw_str_flags_t;

internal r32 draw_str(state_t* state, draw_str_flags_t flags, r32 x_scale_factor, r32 y_scale, r32 start_x, r32 y, str_t str)
{
  r32 x = start_x;
  b32 measure_only = (flags & DRAW_STR_MEASURE_ONLY);

  if(state->font_texture_id)
  {
    if(!measure_only)
    {
      glBindTexture(GL_TEXTURE_2D, state->alpha_blend ? state->font_texture_id : 0);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glEnable(GL_BLEND);
    }

    r32 x_scale = x_scale_factor * y_scale;

    u8* str_end = str.data + str.size;
    i32 last_codepoint = 0;

    for(u8* str_ptr = str.data;
        str_ptr < str_end;
       )
    {
      i32 codepoint = decode_utf8(&str_ptr, str_end);

      if(codepoint == '\t') { codepoint = ' '; }

      i32 x_advance, left_side_bearing;
      stbtt_GetCodepointHMetrics(&state->font, codepoint, &x_advance, &left_side_bearing);

      if(str_ptr != str.data)
      {
        x += x_scale * stbtt_GetCodepointKernAdvance(&state->font, last_codepoint, codepoint) * state->stb_font_scale / state->font_char_w;
      }

      if(!measure_only)
      {
        i32 char_idx = codepoint - state->fixed_codepoint_range_start;
        if(char_idx < 0 || char_idx >= state->fixed_codepoint_range_length)
        {
          char_idx = 0;
          for(i32 custom_idx = 0;
              custom_idx < state->custom_codepoint_count;
              ++custom_idx)
          {
            if(state->custom_codepoints[custom_idx] == codepoint)
            {
              char_idx = custom_idx + state->fixed_codepoint_range_length;
              break;
            }
          }

          if(!char_idx)
          {
            char_idx = state->next_custom_codepoint_idx + state->fixed_codepoint_range_length;
            state->custom_codepoints[state->next_custom_codepoint_idx] = codepoint;

            i32 row = char_idx / state->chars_per_font_row;
            i32 col = char_idx % state->chars_per_font_row;

            // printf("uploading codepoint %d\n", codepoint);
            // printf("  char_idx %d, row %d, col %d\n", char_idx, row, col);

#if 1
            u8* texels_start = state->font_texels + row * state->font_char_h * state->font_texture_w + col * state->font_char_w;
            for_count(j, state->font_char_h)
            {
              for_count(i, state->font_char_w)
              {
                texels_start[j * state->font_texture_w + i] = 0;
              }
            }
            stbtt_MakeCodepointBitmap(&state->font, texels_start,
                state->font_char_w, state->font_char_h, state->font_texture_w, state->stb_font_scale, state->stb_font_scale, codepoint);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, state->font_texture_w);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                col * state->font_char_w, row * state->font_char_h, state->font_char_w, state->font_char_h,
                GL_ALPHA, GL_UNSIGNED_BYTE, texels_start);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);
#else
            u8* tmp_texels = malloc_array(state->font_char_w * state->font_char_h, u8);
            zero_bytes(state->font_char_w * state->font_char_h, tmp_texels);
            stbtt_MakeCodepointBitmap(&state->font, tmp_texels,
                state->font_char_w, state->font_char_h, state->font_char_w, state->stb_font_scale, state->stb_font_scale, codepoint);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                col * state->font_char_w, row * state->font_char_h, state->font_char_w, state->font_char_h,
                GL_ALPHA, GL_UNSIGNED_BYTE, tmp_texels);
            free(tmp_texels);
#endif

            state->next_custom_codepoint_idx = (state->next_custom_codepoint_idx + 1) % state->custom_codepoint_count;
          }
        }

        i32 ix0, iy0, ix1, iy1;
        stbtt_GetCodepointBitmapBox(&state->font, codepoint, state->stb_font_scale, state->stb_font_scale, &ix0, &iy0, &ix1, &iy1);

        r32 x0 = x + x_scale * (r32)left_side_bearing * state->stb_font_scale / state->font_char_w;
        r32 x1 = x0 + x_scale * (r32)(ix1 - ix0) / (r32)state->font_char_w;
        r32 y1 = y - y_scale * (r32)iy0 / (r32)state->font_char_h;
        r32 y0 = y1 - y_scale * (r32)(iy1 - iy0) / (r32)state->font_char_h;

        r32 u0 = (r32)(char_idx % state->chars_per_font_row) / (r32)state->chars_per_font_row;
        r32 u1 = u0 + (r32)(ix1 - ix0) / (r32)state->font_texture_w;
        r32 v0 = (r32)(char_idx / state->chars_per_font_row) / (r32)state->chars_per_font_col;
        r32 v1 = v0 + (r32)(iy1 - iy0) / (r32)state->font_texture_h;

        glBegin(GL_QUADS);
        glTexCoord2f(u0, v1); glVertex2f(x0, y0);
        glTexCoord2f(u1, v1); glVertex2f(x1, y0);
        glTexCoord2f(u1, v0); glVertex2f(x1, y1);
        glTexCoord2f(u0, v0); glVertex2f(x0, y1);
        glEnd();
      }

      x += x_scale * x_advance * state->stb_font_scale / state->font_char_w;
      last_codepoint = codepoint;
    }
  }

  return x - start_x;
}

internal void draw_wrapped_text(state_t* state,
    draw_str_flags_t draw_str_flags, r32 fs, r32 x0, r32 x1, r32* x, r32* y, str_t text)
{
  u8* text_end = text.data + text.size;
  r32 max_word_width = x1 - x0;

  for(u8* word_start = text.data;
      word_start < text_end; // && fs * state->font_ascent + *y >= 0;
     )
  {
    b32 end_line = false;
    u8* word_end = word_start;
    r32 word_width = 0;

    while(word_end < text_end)
    {
      u8* prefix_end = word_end + 1;
      if(!is_ascii(*word_end))
      {
        while(prefix_end < text_end && is_utf8_continuation_byte(*prefix_end))
        {
          ++prefix_end;
        }
      }
      str_t prefix = str_from_span(word_start, prefix_end);
      r32 prefix_width = draw_str(state, DRAW_STR_MEASURE_ONLY, 1, fs, 0, 0, prefix);
      if(word_end > word_start && prefix_width > max_word_width)
      {
        break;
      }
      word_end = prefix_end;

      if(word_end[-1] == '\n')
      {
        end_line = true;
        break;
      }
      else if(word_end[-1] == ' ')
      {
        break;
      }
      else if(is_linewrap_word_separator(word_end[-1]))
      {
        word_width = prefix_width;
        break;
      }

      word_width = prefix_width;
    }

    if(*x + word_width > x1)
    {
      *x = x0;
      *y -= fs;
      while(word_start < text_end && *word_start == ' ')
      {
        ++word_start;
      }
    }

    str_t word = str_from_span(word_start, word_end);
    if(word_end[-1] == '\n') { --word.size; }
    *x += draw_str(state, draw_str_flags, 1, fs, *x, *y, word);
    word_start = word_end;

    if(end_line)
    {
      *x = x0;
      *y -= fs;
    }
  }
}

int main(int argc, char** argv)
{
  debug_out = fopen("/tmp/i2x-debug.log", "wb");
  // if(!debug_out) { debug_out = stderr; }

  Display* display = XOpenDisplay(0);
  if(display)
  {
    int screen_number = 0;
    Window root_window = RootWindow(display, screen_number);

    b32 xi_available = false;
    int xi_opcode = 0;
#if 1
    {
      int query_event = 0;
      int query_error = 0;
      if(XQueryExtension(display, "XInputExtension", &xi_opcode, &query_event, &query_error))
      {
        int major = 2;
        int minor = 1;
        if(XIQueryVersion(display, &major, &minor) == Success)
        {
          // printf("XInput %d.%d available.\n", major, minor);
          xi_available = true;
        }
      }
    }
    if(!xi_available)
    {
      fprintf(stderr, "No XInput2 available.\n");
    }
#endif

    int glx_major = 0;
    int glx_minor = 0;
    if(glXQueryVersion(display, &glx_major, &glx_minor))
    {
      // printf("GLX version: %d.%d\n", glx_major, glx_minor);
      // printf("GLX extensions: %s\n", glXQueryExtensionsString(display, screen_number));

      // glXSwapIntervalEXT = (PFNGLXSWAPINTERVALEXTPROC)
      //   glXGetProcAddress((const GLubyte*)"glXSwapIntervalEXT");

      int glx_config_attrib_list[] = {
        GLX_DOUBLEBUFFER,   1,
        GLX_RED_SIZE,       8,
        GLX_GREEN_SIZE,     8,
        GLX_BLUE_SIZE,      8,
        // GLX_DEPTH_SIZE,    16,
        // GLX_SAMPLE_BUFFERS, 1,
        // GLX_SAMPLES,        4,
        0
      };
      int glx_config_count = 0;
      GLXFBConfig* glx_configs = glXChooseFBConfig(display, screen_number,
          glx_config_attrib_list, &glx_config_count);
      if(glx_configs && glx_config_count > 0)
      {
        GLXFBConfig glx_config = glx_configs[0];

#if 0
        printf("%d GLX configs available.\n", glx_config_count);
        for(i32 i = 0;
            i < glx_config_count;
            ++i)
        {
          printf("\n%d:\n", i);
          int tmp;
#define PRATTR(x) glXGetFBConfigAttrib(display, glx_configs[i], x, &tmp); printf("  " #x ": %d\n", tmp);
          PRATTR(GLX_DOUBLEBUFFER);
          PRATTR(GLX_RED_SIZE);
          PRATTR(GLX_GREEN_SIZE);
          PRATTR(GLX_BLUE_SIZE);
          PRATTR(GLX_DEPTH_SIZE);
          PRATTR(GLX_SAMPLE_BUFFERS);
          PRATTR(GLX_SAMPLES);
#undef PRATTR
        }
#endif

        XFree(glx_configs);
        GLXContext glx_context = glXCreateNewContext(display, glx_config, GLX_RGBA_TYPE, 0, True);
        if(!glXIsDirect(display, glx_context))
        {
          fprintf(stderr, "GLX context is not direct.  Rendering may be slow.\n");
        }

        XVisualInfo* visual_info = glXGetVisualFromFBConfig(display, glx_config);
        Visual* glx_visual = visual_info->visual;

        XSetWindowAttributes window_attributes = {0};
        window_attributes.event_mask = 0
          | KeyPressMask
          | KeyReleaseMask
          // | EnterWindowMask
          // | LeaveWindowMask
          // | KeymapStateMask
          | StructureNotifyMask
          | FocusChangeMask
          | ExposureMask
          ;
        if(!xi_available)
        {
          window_attributes.event_mask = window_attributes.event_mask
            | ButtonPressMask
            | ButtonReleaseMask
            | PointerMotionMask
            ;
        }
        window_attributes.colormap = XCreateColormap(display, root_window, glx_visual, AllocNone);

        Window window = XCreateWindow(display, root_window,
            0, 0, WINDOW_INIT_W, WINDOW_INIT_H,
            0, visual_info->depth, InputOutput, glx_visual,
            CWEventMask | CWColormap, &window_attributes);

        XFree(visual_info);
        GLXWindow glx_window = glXCreateWindow(display, glx_config, window, 0);

        state_t _state = {0};
        state_t* state = &_state;
        state->vsync = true;

        // TODO: Check if GLX_EXT_swap_control is in the extensions string first.
        // Enable VSync.
        glXSwapIntervalEXT(display, glx_window, state->vsync);

        if(xi_available)
        {
          XIEventMask window_evmask;
          XIEventMask root_evmask;
          u8 mask[(XI_LASTEVENT + 7) / 8];

          zero_struct(mask);
          XISetMask(mask, XI_ButtonPress);
          XISetMask(mask, XI_ButtonRelease);
          XISetMask(mask, XI_Motion);
          XISetMask(mask, XI_DeviceChanged);

          window_evmask.deviceid = 2;  // 2: Master pointer.
          window_evmask.mask_len = sizeof(mask);
          window_evmask.mask = mask;
          XISelectEvents(display, window, &window_evmask, 1);

          zero_struct(mask);
          XISetMask(mask, XI_RawMotion);
          XISetMask(mask, XI_Motion);  // This gets scroll events over the title bar.

          root_evmask.deviceid = 2;  // 2: Master pointer.
          root_evmask.mask_len = sizeof(mask);
          root_evmask.mask = mask;
          XISelectEvents(display, root_window, &root_evmask, 1);
        }

        set_title(display, window, (unsigned char*)PROGRAM_NAME, sizeof(PROGRAM_NAME) - 1);
        XMapWindow(display, window);

#if 0
        Cursor empty_cursor;
        {
          char zero = 0;
          Pixmap empty_pixmap = XCreateBitmapFromData(display, root_window, &zero, 1, 1);
          XColor empty_color = {0};
          empty_cursor = XCreatePixmapCursor(display, empty_pixmap, empty_pixmap,
              &empty_color, &empty_color, 0, 0);
          XFreePixmap(display, empty_pixmap);
        }
#endif

        Atom atom_clipboard = XInternAtom(display, "CLIPBOARD", false);
        Atom atom_targets = XInternAtom(display, "TARGETS", false);
        Atom atom_incr = XInternAtom(display, "INCR", false);
        Atom atom_utf8 = XInternAtom(display, "UTF8_STRING", false);
        Atom atom_uri_list = XInternAtom(display, "text/uri-list", false);
        Atom atom_mycliptarget = XInternAtom(display, "PUT_IT_HERE", false);

#if 1
        // This is needed for XmbLookupString to return UTF-8.
        if(!setlocale(LC_ALL, "en_US.UTF-8"))
        {
          fprintf(stderr, "Could not set locale to \"en_US.UTF-8\".\n");
          setlocale(LC_ALL, "");
        }
#endif
        // printf("%d\n", XSupportsLocale());
#if 0
        // This says that this is needed for dead keys, but apparently not?
        // https://stackoverflow.com/a/18288346
        if(!XSetLocaleModifiers("@im=none"))
        {
          fprintf(stderr, "XSetLocaleModifiers(\"@im=none\") failed.\n");
        }
#endif
        XIM x_input_method = XOpenIM(display, 0, 0, 0);
        // puts(XLocaleOfIM(x_input_method));
        XIC x_input_context = 0;
        if(x_input_method)
        {
          x_input_context = XCreateIC(x_input_method,
              XNInputStyle, XIMPreeditNothing | XIMStatusNothing,
              XNClientWindow, window,
              NULL);
          if(!x_input_context)
          {
            fprintf(stderr, "X Input Context could not be created!\n");
          }
        }
        else
        {
          fprintf(stderr, "X Input Method could not be opened!\n");
        }
        // XSetICFocus(x_input_context);

        glXMakeContextCurrent(display, glx_window, glx_window, glx_context);

#if 0
        state->inotify_fd = inotify_init1(IN_NONBLOCK);
#else
        state->inotify_fd = -1;
#endif

        state->input_path_count = argc - 1;
        state->input_paths = malloc_array(state->input_path_count, char*);
        for(i32 input_idx = 0;
            input_idx < state->input_path_count;
            ++input_idx)
        {
          state->input_paths[input_idx] = argv[input_idx + 1];
        }
        str_t open_single_directory_on = {0};
        if(state->input_path_count == 1)
        {
          char* arg = state->input_paths[0];
          if(!is_directory(arg))
          {
            char* arg_dir_end = arg + zstr_length(arg);
            while(arg_dir_end > arg + 1 && arg_dir_end[0] != '/')
            {
              --arg_dir_end;
            }

            open_single_directory_on = wrap_str(arg);
            if(arg_dir_end[0] != '/')
            {
              state->input_paths[0] = ".";
            }
            else
            {
              state->input_paths[0] = strndup(arg, arg_dir_end - arg);
            }

#if 0
            printf("Opening single directory '%s' at '%.*s'\n",
                state->input_paths[0],
                (int)open_single_directory_on.size, open_single_directory_on.data);
#endif
          }
        }

        state->total_img_capacity = max(state->input_path_count, 128 * 1024);
        state->img_entries = malloc_array(state->total_img_capacity, img_entry_t);
        zero_bytes(state->total_img_capacity * sizeof(img_entry_t), state->img_entries);
        state->filtered_img_idxs = malloc_array(state->total_img_capacity, i32);

        reload_input_paths(state);

        if(open_single_directory_on.size)
        {
          for_count(img_idx, state->total_img_count)
          {
            if(str_has_suffix(state->img_entries[img_idx].path, open_single_directory_on))
            {
              state->viewing_filtered_img_idx = img_idx;
              break;
            }
          }
        }

        glGenTextures(1, &state->test_texture_id);
        glBindTexture(GL_TEXTURE_2D, state->test_texture_id);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
            test_texture_w, test_texture_h, 0, GL_RGBA, GL_UNSIGNED_BYTE, test_texels);
        glGenerateMipmap(GL_TEXTURE_2D);

        state->font_texture_w = 512;
        state->font_texture_h = 512;
        state->font_char_w = 32;
        state->font_char_h = 32;
        state->chars_per_font_row = state->font_texture_w / state->font_char_w;
        state->chars_per_font_col = state->font_texture_h / state->font_char_h;
        state->fixed_codepoint_range_start = 32;  // ' '
        state->fixed_codepoint_range_length = 127 - state->fixed_codepoint_range_start;  // '~'
        state->custom_codepoint_count = state->chars_per_font_row * state->chars_per_font_col - state->fixed_codepoint_range_length;
        state->custom_codepoints = malloc_array(state->custom_codepoint_count, u32);
        for_count(i, state->custom_codepoint_count) { state->custom_codepoints[i] = ' '; }
        // TODO: Pack a font into the executable.
        char* ttf_paths[] = {
          "/usr/share/fonts/TTF/DejaVuSans.ttf",
          "/usr/share/fonts/TTF/Vera.ttf",
        };
        for(i32 ttf_idx = 0;
            ttf_idx < array_count(ttf_paths) && !state->font_texture_id;
            ++ttf_idx)
        {
          char* ttf_path = ttf_paths[ttf_idx];
          str_t ttf_contents = read_file(ttf_path);

          if(ttf_contents.size > 0)
          {
            if(stbtt_InitFont(&state->font, ttf_contents.data, stbtt_GetFontOffsetForIndex(ttf_contents.data, 0)))
            {
              state->font_texels = malloc_array(state->font_texture_w * state->font_texture_h, u8);
              zero_bytes(state->font_texture_w * state->font_texture_h, state->font_texels);

              state->stb_font_scale = stbtt_ScaleForPixelHeight(&state->font, 32);

              int ascent, descent, line_gap;
              stbtt_GetFontVMetrics(&state->font, &ascent, &descent, &line_gap);
              state->font_ascent = (r32)ascent * state->stb_font_scale / (r32)state->font_char_h;
              state->font_descent = -(r32)descent * state->stb_font_scale / (r32)state->font_char_h;

              for(i32 char_idx = 0;
                  char_idx < state->fixed_codepoint_range_length;
                  ++char_idx)
              {
                i32 row = char_idx / state->chars_per_font_row;
                i32 col = char_idx % state->chars_per_font_row;

                i32 codepoint = state->fixed_codepoint_range_start + char_idx;

                stbtt_MakeCodepointBitmap(&state->font,
                    state->font_texels + row * state->font_char_h * state->font_texture_w + col * state->font_char_w,
                    state->font_char_w, state->font_char_h, state->font_texture_w, state->stb_font_scale, state->stb_font_scale, codepoint);
              }

              glGenTextures(1, &state->font_texture_id);
              glBindTexture(GL_TEXTURE_2D, state->font_texture_id);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
              glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
              glTexImage2D(GL_TEXTURE_2D, 0, GL_ALPHA,
                  state->font_texture_w, state->font_texture_h, 0, GL_ALPHA, GL_UNSIGNED_BYTE, state->font_texels);
            }
            else
            {
              free(ttf_contents.data);
            }
          }

          if(!state->font_texture_id)
          {
            fprintf(stderr, "Could not generate font from '%s'.\n", ttf_path);
          }
        }

        pthread_t loader_threads[7] = {0};
        i32 loader_count = array_count(loader_threads);
        shared_loader_data_t* shared_loader_data = malloc_array(1, shared_loader_data_t);
        loader_data_t* loader_data = malloc_array(loader_count, loader_data_t);
        sem_t* loader_semaphores = malloc_array(loader_count, sem_t);

        zero_struct(*shared_loader_data);
        shared_loader_data->total_img_count = state->total_img_count;
        shared_loader_data->img_entries = state->img_entries;
        shared_loader_data->filtered_img_count = state->filtered_img_count;
        shared_loader_data->filtered_img_idxs = state->filtered_img_idxs;

        for(i32 loader_idx = 0;
            loader_idx < loader_count;
            ++loader_idx)
        {
          sem_init(&loader_semaphores[loader_idx], 0, 0);
          loader_data[loader_idx].thread_idx = loader_idx + 1;
          loader_data[loader_idx].semaphore = &loader_semaphores[loader_idx];
          loader_data[loader_idx].shared = shared_loader_data;
          pthread_create(&loader_threads[loader_idx], 0, loader_fun, &loader_data[loader_idx]);
        }

        state->win_w = WINDOW_INIT_W;
        state->win_h = WINDOW_INIT_H;

        r32 time = 0;
        u32 frames_since_last_print = 0;
        u64 nsecs_last_print = get_nanoseconds();
        u64 nsecs_last_frame = nsecs_last_print;
        i64 nsecs_min = I64_MAX;
        i64 nsecs_max = I64_MIN;

        b32 quitting = false;
        i32 last_viewing_img_idx = -1;
        str_t clipboard_str = {0};
        b32 border_sampling = true;
        state->linear_sampling = true;
        state->alpha_blend = true;
        b32 bright_bg = false;
        b32 show_fps = false;
        r32 zoom = 0;
        r32 offset_x = 0;
        r32 offset_y = 0;
        state->hide_sidebar = false;
        state->sidebar_width = 310;
        state->thumbnail_columns = 2;
        i32 hovered_thumbnail_idx = -1;
        state->show_info = 2;
        state->search_str_capacity = 1024;
        state->search_str.data = malloc_array(state->search_str_capacity, u8);
        state->info_panel_width = 500;

        state->xi_scroll_x_increment = 120.0f;
        state->xi_scroll_y_increment = 120.0f;

        r32 offset_scroll_scale = 0.125f;
        r32 zoom_scroll_scale = 0.25f;

        r32 prev_mouse_x = 0;
        r32 prev_mouse_y = 0;
        b32 lmb_held = false;
        b32 mmb_held = false;
        b32 rmb_held = false;
        b32 shift_held = false;
        b32 ctrl_held = false;
        b32 alt_held = false;
        b32 has_focus = false;
        i32 dirty_frames = 1;
        b32 scroll_thumbnail_into_view = !!open_single_directory_on.size;

        ui_interaction_t hovered_interaction = {0};
        ui_interaction_t current_interaction = {0};
        ui_interaction_t mainview_interaction = { &offset_x };
        ui_interaction_t sidebar_interaction = { &state->viewing_filtered_img_idx };
        ui_interaction_t sidebar_resize_interaction = { &state->sidebar_width };
        ui_interaction_t scrollbar_interaction = { &state->sidebar_scroll_rows };
        ui_interaction_t info_panel_resize_interaction = { &state->info_panel_width };

        while(!quitting)
        {
#if 1
          if(state->vsync)
          {
            // TODO: Check for this in GLX extension string before using it.
            glXDelayBeforeSwapNV(display, glx_window, 0.002f);
          }
#endif

          b32 dirty = false;
          b32 reloaded_paths = false;
          b32 search_changed = false;

          if(!state->vsync) { dirty = true; }

          if(state->inotify_fd != -1)
          {
            u8 notification_buffer[sizeof(struct inotify_event) + NAME_MAX + 1] = {0};

            ssize_t bytes_available = read(state->inotify_fd, notification_buffer, sizeof(notification_buffer));
            u8* notifications_end = notification_buffer + bytes_available;
            u8* notification_ptr = notification_buffer;
            b32 got_notification = false;
            while(notification_ptr + sizeof(struct inotify_event) <= notifications_end)
            {
              got_notification = true;

              struct inotify_event* notification = (void*)notification_ptr;
              ssize_t notification_size = sizeof(struct inotify_event) + notification->len;

#if 0
              // printf("%ld / %lu bytes read\n", bytes_available, sizeof(struct inotify_event));
              printf("inotify read:\n");
              printf("  wd:     %d\n", notification->wd);
              printf("  mask:   0x%08x\n", notification->mask);
              printf("  cookie: %u\n", notification->cookie);
              printf("  len:    %u\n", notification->len);
              if(notification->name[0] != 0)
              {
                printf("  name:   %.*s\n", (int)notification->len, notification->name);
              }
#endif

              notification_ptr += notification_size;
            }

            if(got_notification)
            {
              reload_input_paths(state);
              reloaded_paths = true;
              dirty = true;
            }
          }

          i32 info_height = 0;
          r32 win_min_side = 0;
          r32 fs = 0;
          i32 effective_sidebar_width = 0;
          i32 effective_info_panel_width = 0;
          i32 image_region_x0 = 0;
          i32 image_region_y0 = 0;
          i32 image_region_w = 0;
          i32 image_region_h = 0;

          for(;;)
          {
            win_min_side = min(state->win_w, state->win_h);
            fs = clamp(12, 36, (26.0f / 1080.0f) * win_min_side);
            info_height = 1.2f * fs;
            effective_sidebar_width = get_effective_sidebar_width(state);
            effective_info_panel_width = max(1, min(state->win_w - 10, state->info_panel_width));
            image_region_x0 = state->hide_sidebar ? 0 : effective_sidebar_width;
            image_region_y0 = (state->show_info == 1) ? info_height : 0;
            image_region_w = max(0, state->win_w - image_region_x0 - (state->show_info == 2 ? effective_info_panel_width : 0));
            image_region_h = max(0, state->win_h - image_region_y0);

            if(!XPending(display)) { break; }

            r32 mouse_x = prev_mouse_x;
            r32 mouse_y = prev_mouse_y;
            r32 mouse_delta_x = 0;
            r32 mouse_delta_y = 0;
            i32 scroll_y_ticks = 0;
            r32 scroll_x = 0;
            r32 scroll_y = 0;
            i32 mouse_btn_went_down = 0;
            i32 mouse_btn_went_up = 0;
            r32 thumbnail_h = get_thumbnail_size(state);

            XEvent event;
            XNextEvent(display, &event);

            // This is needed for XIM to handle combining keys, like ` + a = à.
            if(XFilterEvent(&event, None)) { continue; }

            if(event.xcookie.type == GenericEvent && event.xcookie.extension == xi_opcode)
            {
              if(XGetEventData(display, &event.xcookie))
              {
                // printf("XI event type %d\n", event.xcookie.evtype);
                switch(event.xcookie.evtype)
                {
                  case XI_ButtonPress:
                  case XI_ButtonRelease:
                  case XI_Motion:
                  {
                    XIDeviceEvent* devev = (XIDeviceEvent*)event.xcookie.data;
                    u32 button = devev->detail;
                    u32 button_mask = devev->buttons.mask_len > 0 ? devev->buttons.mask[0] : 0;
                    i32 mods = devev->mods.effective;
                    shift_held = (mods & 1);
                    ctrl_held = (mods & 4);
                    alt_held = (mods & 8);
                    mouse_x = (r32)devev->event_x;
                    mouse_y = (r32)state->win_h - (r32)devev->event_y - 1.0f;

                    // Getting the smooth scroll deltas with XInput2 is tricky.
                    //
                    // XI_RawMotion does deliver them directly, but those can only be listened to
                    // for the entire root X window, which means they're delivered even when the
                    // window is out of focus.
                    //
                    // Tracking focus manually is also problematic, because (XI_)FocusIn/FocusOut
                    // events get delivered (at least in i3) when the window gets focused but the
                    // mouse cursor is on the title bar (among other edge cases, like another
                    // window being on top of the focused window), even though normally the window
                    // wouldn't be receiving mouse events in this case, which means the
                    // window-local mouse position doesn't get delivered, leading to other problems.
                    //
                    // Using XI_Motion events (as probably intended) is very problematic because the
                    // scroll values get accumulated (instead of getting delivered as deltas), even
                    // while our window is out of focus.
                    // Additionally, since the cumulative double-precision floating point number
                    // could grow large and run out of precision, it can get wrapped sometimes,
                    // allegedly.
                    // However, because of the focusing issues with XI_RawMotion, this is still
                    // the solution that I decided to implement here.
                    //
                    // The current accumulated scroll value can be queried with XIQueryDevice
                    // (see the FocusIn event handler), and gets automatically delivered with
                    // XI_DeviceChanged events, e.g. when the user starts using a different mouse.
                    //
                    // But because FocusIn gets delivered when the mouse is on the title bar,
                    // and XI_Motion events only appear for our window when the mouse is
                    // on the actual contents of the window, we might still lose changes to
                    // the accumulated scroll value while the cursor is on the title bar.
                    // For this reason, we register for XI_Motion events on both the root window
                    // and our window, which seems to have the effect of delivering these events
                    // while the cursor is on the title bar, but only while the window is focused,
                    // which is what we want, but we do have to check whether the XI_Motion event
                    // is for the root window or ours here.
                    b32 inside_window = (devev->event == window);

                    DEBUG_LOG("%s %d:%d detail %d flags %d mods %d r %.2f,%.2f e %.2f,%.2f btns %d\n",
                        event.xcookie.evtype == XI_ButtonPress ? "XI_ButtonPress"
                        : event.xcookie.evtype == XI_ButtonRelease ? "XI_ButtonRelease" : "XI_Motion",
                        devev->deviceid, devev->sourceid,
                        devev->detail, devev->flags,
                        devev->mods.effective,
                        devev->root_x, devev->root_y, devev->event_x, devev->event_y,
                        button_mask);
                    DEBUG_LOG("  valuators: mask_len %d mask 0x", devev->valuators.mask_len);
                    for_count(i, devev->valuators.mask_len) { DEBUG_LOG("%02x", devev->valuators.mask[i]); }
                    DEBUG_LOG("\n");
                    {
                      r64* values = devev->valuators.values;
                      for_count(i, devev->valuators.mask_len)
                      {
                        for_count(b, 8)
                        {
                          if(devev->valuators.mask[i] & (1 << b))
                          {
                            u32 idx = 8 * i + b;
                            DEBUG_LOG("    %u: %.2f\n", idx, *values);
                            ++values;
                          }
                        }
                      }
                    }

                    if(devev->valuators.mask_len >= 1)
                    {
                      u8 mask = devev->valuators.mask[0];
                      r64* value_ptr = devev->valuators.values;

                      for_count(bit_idx, 4)
                      {
                        if(mask & (1 << bit_idx))
                        {
                          if(bit_idx == 2)
                          {
                            r32* last = &state->xi_last_scroll_x_valuator;
                            r32 delta = *value_ptr - *last;
                            if(absolute(delta < 1e6f) && inside_window)
                            {
                              scroll_x += delta / state->xi_scroll_x_increment;
                            }
                            *last = *value_ptr;
                          }
                          else if(bit_idx == 3)
                          {
                            r32* last = &state->xi_last_scroll_y_valuator;
                            r32 delta = *value_ptr - *last;
                            if(absolute(delta < 1e6f) && inside_window)
                            {
                              scroll_y -= delta / state->xi_scroll_y_increment;
                            }
                            *last = *value_ptr;
                          }
                          ++value_ptr;
                        }
                      }
                    }

                    if(event.xcookie.evtype == XI_Motion)
                    {
                      lmb_held = (button_mask & 2);
                      mmb_held = (button_mask & 4);
                      rmb_held = (button_mask & 8);
                    }
                    else
                    {
                      b32 went_down = (event.xcookie.evtype == XI_ButtonPress);
                      if(button == 1)
                      {
                        lmb_held = went_down;
                      }
                      else if(button == 2)
                      {
                        mmb_held = went_down;
                      }
                      else if(button == 3)
                      {
                        rmb_held = went_down;
                      }

                      if(inside_window)
                      {
                        if(went_down)
                        {
                          mouse_btn_went_down = button;
                          if(button == 4)
                          {
                            scroll_y_ticks += 1;
                          }
                          else if(button == 5)
                          {
                            scroll_y_ticks -= 1;
                          }
                          if(!(devev->flags & XIPointerEmulated))
                          {
                            if(button == 4)
                            {
                              scroll_y += 1.0f;
                            }
                            else if(button == 5)
                            {
                              scroll_y -= 1.0f;
                            }
                            else if(button == 6)
                            {
                              scroll_x -= 1.0f;
                            }
                            else if(button == 7)
                            {
                              scroll_x += 1.0f;
                            }
                          }

                          state->dragging_start_x = mouse_x;
                          state->dragging_start_y = mouse_y;
                        }
                        else
                        {
                          mouse_btn_went_up = button;
                        }
                      }
                    }

                    // TODO: This probably needs to happen for absolute pointers, e.g. tablet devices.
                    // mouse_delta_x += mouse_x - prev_mouse_x;
                    // mouse_delta_y += mouse_y - prev_mouse_y;
                  } break;

                  case XI_RawMotion:
                  {
                    if(has_focus)
                    {
                      XIRawEvent* devev = (XIRawEvent*)event.xcookie.data;

#if 0
                      printf("XI raw motion %d:%d detail %d flags %d\n",
                          devev->deviceid, devev->sourceid, devev->detail, devev->flags);
                      printf("  valuators: mask_len %d mask 0x", devev->valuators.mask_len);
                      for_count(i, devev->valuators.mask_len) { printf("%02x", devev->valuators.mask[i]); }
                      printf("\n");
#endif
                      r64* values = devev->valuators.values;
                      r64* raw_values = devev->raw_values;
                      for_count(i, devev->valuators.mask_len)
                      {
                        for_count(b, 8)
                        {
                          if(devev->valuators.mask[i] & (1 << b))
                          {
                            u32 idx = 8 * i + b;
                            r32 value = (r32)*values;
#if 0
                            printf("    %u: %6.2f (raw %.2f)\n", idx, value, *raw_values);
#endif

                            // TODO: Find out the correct source IDs.  Maybe using this:
                            //       http://who-t.blogspot.com/2009/06/xi2-recipies-part-2.html
                            // if(devev->sourceid == 9 || devev->sourceid == 11 || devev->sourceid == 12 || devev->sourceid == 13)
                            {
                              if(idx == 0)
                              {
                                mouse_delta_x += value;
                              }
                              else if(idx == 1)
                              {
                                mouse_delta_y -= value;
                              }
#if 0
                              else if(idx == 2)
                              {
                                scroll_x += value / state->xi_scroll_x_increment;
                              }
                              else if(idx == 3)
                              {
                                scroll_y -= value / state->xi_scroll_y_increment;
                              }
#endif
                            }

                            ++values;
                            ++raw_values;
                          }
                        }
                      }
                    }
                  } break;

                  case XI_DeviceChanged:
                  {
                    XIDeviceChangedEvent* device = (XIDeviceChangedEvent*)event.xcookie.data;
                    DEBUG_LOG("\nXI Device %d changed\n", device->deviceid);
                    xi_update_device_info(state, device->num_classes, device->classes);
                  } break;
                }
                XFreeEventData(display, &event.xcookie);
              }
            }
            else
            {
              // printf("Core event type %d\n", event.type);
              switch(event.type)
              {
                case KeyPress:
                case KeyRelease:
                {
                  b32 went_down = (event.type == KeyPress);
                  u32 keycode = event.xkey.keycode;
                  KeySym keysym = XLookupKeysym(&event.xkey, 0);
                  shift_held = (event.xkey.state & 1);
                  ctrl_held = (event.xkey.state & 4);
                  alt_held = (event.xkey.state & 8);
                  lmb_held = (event.xkey.state & 0x100);
                  mmb_held = (event.xkey.state & 0x200);
                  rmb_held = (event.xkey.state & 0x400);

#if 0
                  printf("state %#x keycode %u keysym %#lx (%s) %s\n",
                      event.xkey.state, keycode, keysym, XKeysymToString(keysym),
                      went_down ? "Pressed" : "Released");
#endif

                  if(went_down)
                  {
                    if(ctrl_held && keysym == 'q')
                    {
                      quitting = true;
                    }
                    else if(ctrl_held && keysym == 'f' || !state->searching && keysym == '/')
                    {
                      bflip(state->searching);
                      if(state->searching)
                      {
                        state->selection_start = state->search_str.size;
                        state->selection_end   = state->search_str.size;
                        state->img_idx_viewed_before_search = state->filtered_img_idxs[state->viewing_filtered_img_idx];
                        search_changed = true;
                      }
                    }
                    else if(state->searching)
                    {
                      if(keysym == XK_Escape || keysym == XK_Return)
                      {
                        state->searching = false;
                      }
                      else if(ctrl_held && keysym == 'a')
                      {
                        str_t* str = &state->search_str;

                        state->selection_start = 0;
                        state->selection_end = str->size;
                      }
                      else if(keysym == XK_BackSpace)
                      {
                        if(state->selection_start == state->selection_end)
                        {
                          state->selection_start = seek_left_in_str(
                              state->search_str, ctrl_held, state->selection_start);
                        }
                        str_replace_selection(state->search_str_capacity, &state->search_str,
                            &state->selection_start, &state->selection_end, str(""));
                        search_changed = true;
                      }
                      else if(keysym == XK_Delete)
                      {
                        if(state->selection_start == state->selection_end)
                        {
                          state->selection_end = seek_right_in_str(
                              state->search_str, ctrl_held, state->selection_end);
                        }
                        str_replace_selection(state->search_str_capacity, &state->search_str,
                            &state->selection_start, &state->selection_end, str(""));
                        search_changed = true;
                      }
                      else if(keysym == XK_Left)
                      {
                        state->selection_end = seek_left_in_str(state->search_str, ctrl_held, state->selection_end);
                        if(!shift_held)
                        {
                          state->selection_start = state->selection_end;
                        }
                      }
                      else if(keysym == XK_Right)
                      {
                        state->selection_end = seek_right_in_str(state->search_str, ctrl_held, state->selection_end);
                        if(!shift_held)
                        {
                          state->selection_start = state->selection_end;
                        }
                      }
                      else if(keysym == XK_Home)
                      {
                        if(!shift_held)
                        {
                          state->selection_start = 0;
                        }
                        state->selection_end = 0;
                      }
                      else if(keysym == XK_End)
                      {
                        if(!shift_held)
                        {
                          state->selection_start = state->search_str.size;
                        }
                        state->selection_end = state->search_str.size;
                      }
                      else if(!ctrl_held)
                      {
                        Status xmb_lookup_status = 0;
                        u8 entered_buffer[256];
                        str_t entered_str = {entered_buffer};
                        entered_str.size = Xutf8LookupString(x_input_context, &event.xkey,
                        // entered_str.size = XmbLookupString(x_input_context, &event.xkey,
                            (char*)entered_buffer, sizeof(entered_buffer),
                            0, &xmb_lookup_status);
                        // printf("%d %lu\n", xmb_lookup_status, entered_str.size);
                        if(xmb_lookup_status != XBufferOverflow && entered_str.size > 0)
                        {
#if 0
                          printf("\n");
                          for_count(i, entered_str.size)
                          {
                            printf("0x%04x '%c'\n", entered_str.data[i], entered_str.data[i]);
                          }
#endif

                          if(str_replace_selection(state->search_str_capacity, &state->search_str,
                                &state->selection_start, &state->selection_end, entered_str))
                          {
                            search_changed = true;
                          }
                        }
                      }
                    }
                    else if(keysym == XK_Escape)
                    {
                      // TODO: Better to persist the marking-state.
                      b32 any_marked = false;
                      for_count(i, state->total_img_count) { any_marked = any_marked || (state->img_entries[i].flags & IMG_FLAG_MARKED); }
                      if(!any_marked) { quitting = true; }
                    }
                    else if(keysym == XK_Shift_L || keysym == XK_Shift_R)
                    {
                      shift_held = true;
                    }
                    else if(keysym == XK_Control_L || keysym == XK_Control_R)
                    {
                      ctrl_held = true;
                    }
                    else if(keysym == XK_Alt_L || keysym == XK_Alt_R)
                    {
                      alt_held = true;
                    }
                    else if(keysym == XK_BackSpace || keysym == XK_Left || keysym == 'h')
                    {
                      if(state->viewing_filtered_img_idx > 0)
                      {
                        state->viewing_filtered_img_idx -= 1;
                      }
                      scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == ' ' || keysym == XK_Right || keysym == 'l')
                    {
                      if(state->viewing_filtered_img_idx < state->filtered_img_count - 1)
                      {
                        state->viewing_filtered_img_idx += 1;
                      }
                      scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_Up || keysym == 'k')
                    {
                      if(state->viewing_filtered_img_idx - state->thumbnail_columns >= 0)
                      {
                        state->viewing_filtered_img_idx -= state->thumbnail_columns;
                      }
                      scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_Down || keysym == 'j')
                    {
                      if(state->viewing_filtered_img_idx + state->thumbnail_columns < state->filtered_img_count)
                      {
                        state->viewing_filtered_img_idx += state->thumbnail_columns;
                      }
                      scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_Home || (!shift_held && keysym == 'g'))
                    {
                      state->viewing_filtered_img_idx = 0;
                      scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_End || (shift_held && keysym == 'g'))
                    {
                      state->viewing_filtered_img_idx = state->filtered_img_count - 1;
                      scroll_thumbnail_into_view = true;
                    }
                    else if(alt_held && keysym == XK_Page_Up)
                    {
                      state->viewing_filtered_img_idx -= state->thumbnail_columns * (r32)(i32)((r32)state->win_h / thumbnail_h);
                      state->viewing_filtered_img_idx = clamp(0, state->filtered_img_count - 1, state->viewing_filtered_img_idx);
                      scroll_thumbnail_into_view = true;
                    }
                    else if(alt_held && keysym == XK_Page_Down)
                    {
                      state->viewing_filtered_img_idx += state->thumbnail_columns * (r32)(i32)((r32)state->win_h / thumbnail_h);
                      state->viewing_filtered_img_idx = clamp(0, state->filtered_img_count - 1, state->viewing_filtered_img_idx);
                      scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_Page_Up)
                    {
                      state->sidebar_scroll_rows -= (r32)(i32)((r32)state->win_h / thumbnail_h);
                      clamp_sidebar_scroll_rows(state);
                    }
                    else if(keysym == XK_Page_Down)
                    {
                      state->sidebar_scroll_rows += (r32)(i32)((r32)state->win_h / thumbnail_h);
                      clamp_sidebar_scroll_rows(state);
                    }
                    else if(ctrl_held && keysym == 'a')
                    {
                      b32 none_were_marked = true;
                      for_count(i, state->filtered_img_count)
                      {
                        img_entry_t* img = get_filtered_img(state, i);
                        none_were_marked = none_were_marked && !(img->flags & IMG_FLAG_MARKED);
                        img->flags &= ~IMG_FLAG_MARKED;
                      }
                      if(none_were_marked)
                      {
                        for_count(i, state->filtered_img_count)
                        {
                          img_entry_t* img = get_filtered_img(state, i);
                          img->flags |= IMG_FLAG_MARKED;
                        }
                      }
                    }
                    else if(keysym == 'm')
                    {
                      img_entry_t* img = get_filtered_img(state, state->viewing_filtered_img_idx);
                      if(img)
                      {
                        img->flags ^= IMG_FLAG_MARKED;
                      }

                      if(shift_held)
                      {
                        state->viewing_filtered_img_idx += 1;
                        scroll_thumbnail_into_view = true;
                      }
                    }
                    else if(keysym == 'o')
                    {
                      i32 prev_viewing_idx = state->filtered_img_idxs[state->viewing_filtered_img_idx];
                      state->viewing_filtered_img_idx = 0;

                      if(state->filtered_img_count == state->total_img_count)
                      {
                        state->filtered_img_count = 0;
                        for_count(idx, state->total_img_count)
                        {
                          if(state->img_entries[idx].flags & IMG_FLAG_MARKED)
                          {
                            if(prev_viewing_idx >= idx)
                            {
                              state->viewing_filtered_img_idx = state->filtered_img_count;
                            }

                            state->filtered_img_idxs[state->filtered_img_count++] = idx;
                          }
                        }
                      }
                      else
                      {
                        for_count(i, state->total_img_count) { state->filtered_img_idxs[i] = i; }
                        state->filtered_img_count = state->total_img_count;
                        state->viewing_filtered_img_idx = prev_viewing_idx;
                      }
                      clamp_sidebar_scroll_rows(state);
                      scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == 'r')
                    {
                      reload_input_paths(state);
                      reloaded_paths = true;
                    }
                    else if(keysym == 'a')
                    {
                      bflip(border_sampling);
                    }
                    else if(keysym == 'b')
                    {
                      bflip(bright_bg);
                    }
                    else if(keysym == 't')
                    {
                      bflip(state->hide_sidebar);
                    }
                    else if(keysym == 'i')
                    {
                      state->show_info = (state->show_info + 1) % 3;
                    }
                    else if(keysym == 'n')
                    {
                      bflip(state->linear_sampling);
                      for(i32 img_idx = 0;
                          img_idx < state->total_img_count;
                          ++img_idx)
                      {
                        glBindTexture(GL_TEXTURE_2D, state->img_entries[img_idx].texture_id);
                        if(state->linear_sampling)
                        {
                          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                        }
                        else
                        {
                          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        }
                      }
                    }
                    else if(ctrl_held && keysym == 'v')
                    {
                      XConvertSelection(display, atom_clipboard, atom_uri_list, atom_mycliptarget, window, CurrentTime);
                    }
                    else if(ctrl_held && keysym == 'c')
                    {
                      // TODO: Copy list of marked images if there are any.
                      if(state->filtered_img_count)
                      {
                        img_entry_t* img = get_filtered_img(state, state->viewing_filtered_img_idx);
                        if(shift_held)
                        {
                          clipboard_str = img->positive_prompt;
                        }
                        else
                        {
                          clipboard_str = img->path;
                        }
                        XSetSelectionOwner(display, atom_clipboard, window, CurrentTime);
                      }
                    }
                    else if(keysym == 'z')
                    {
                      zoom = 0;
                      offset_x = 0;
                      offset_y = 0;
                      state->zoom_from_original_size = true;
                    }
                    else if(keysym == 'x')
                    {
                      zoom = 0;
                      offset_x = 0;
                      offset_y = 0;
                      state->zoom_from_original_size = false;
                    }
                    else if(keysym == '1')
                    {
                      zoom = 0;
                      state->zoom_from_original_size = true;
                    }
                    else if(keysym == '2')
                    {
                      zoom = shift_held ? -1 : 1;
                      state->zoom_from_original_size = true;
                    }
                    else if(keysym == '3')
                    {
                      zoom = shift_held ? -log2f(3.0f) : log2f(3.0f);
                      state->zoom_from_original_size = true;
                    }
                    else if(keysym == '4')
                    {
                      zoom = shift_held ? -2 : 2;
                      state->zoom_from_original_size = true;
                    }
                    else if(keysym == '0')
                    {
                      if(alt_held)
                      {
                        state->thumbnail_columns = 2;
                      }
                      else
                      {
                        zoom = 0;
                      }
                    }
                    else if(keysym == '-')
                    {
                      if(alt_held)
                      {
                        state->thumbnail_columns += 1;
                        clamp_thumbnail_columns(state);
                        clamp_sidebar_scroll_rows(state);
                      }
                      else
                      {
                        zoom -= 0.25f;
                      }
                    }
                    else if(keysym == '=')
                    {
                      if(alt_held)
                      {
                        state->thumbnail_columns -= 1;
                        clamp_thumbnail_columns(state);
                        clamp_sidebar_scroll_rows(state);
                      }
                      else
                      {
                        zoom += 0.25f;
                      }
                    }
                    else if(keysym == '5')
                    {
                      bflip(show_fps);
                    }
                    else if(keysym == '6')
                    {
                      bflip(state->alpha_blend);
                    }
                    else if(keysym == '7')
                    {
                      bflip(state->vsync);
                      glXSwapIntervalEXT(display, glx_window, (int)state->vsync);
                    }
                  }
                  else
                  {
                    if(keysym == XK_Shift_L || keysym == XK_Shift_R)
                    {
                      shift_held = false;
                    }
                    else if(keysym == XK_Control_L || keysym == XK_Control_R)
                    {
                      ctrl_held = false;
                    }
                    else if(keysym == XK_Alt_L || keysym == XK_Alt_R)
                    {
                      alt_held = false;
                    }
                  }

                  dirty = true;

                  // TODO: Look into XmbLookupString for typed text input.
                  // https://www.x.org/releases/current/doc/libX11/libX11/libX11.html#XmbLookupString
                } break;

                case ButtonPress:
                case ButtonRelease:
                {
                  b32 went_down = (event.type == ButtonPress);
                  u32 button = event.xbutton.button;
                  shift_held = (event.xbutton.state & 1);
                  ctrl_held = (event.xbutton.state & 4);
                  alt_held = (event.xbutton.state & 8);
                  lmb_held = (event.xbutton.state & 0x100);
                  mmb_held = (event.xbutton.state & 0x200);
                  rmb_held = (event.xbutton.state & 0x400);
                  mouse_x = event.xbutton.x;
                  mouse_y = state->win_h - event.xbutton.y - 1;

#if 0
                  printf("state %#x button %u %s\n",
                      event.xbutton.state, button, went_down ? "Pressed" : "Released");
#endif

                  if(went_down)
                  {
                    mouse_btn_went_down = button;
                    if(button == 4)
                    {
                      scroll_y_ticks += 1;
                      scroll_y += 1.0f;
                    }
                    else if(button == 5)
                    {
                      scroll_y_ticks -= 1;
                      scroll_y -= 1.0f;
                    }
                    else if(button == 6)
                    {
                      scroll_x -= 1.0f;
                    }
                    else if(button == 7)
                    {
                      scroll_x += 1.0f;
                    }

                    state->dragging_start_x = mouse_x;
                    state->dragging_start_y = mouse_y;
                  }
                } break;

                case MotionNotify:
                {
                  // printf("%x\n", event.xmotion.state);
                  shift_held = (event.xmotion.state & 1);
                  ctrl_held = (event.xmotion.state & 4);
                  alt_held = (event.xmotion.state & 8);
                  lmb_held = (event.xmotion.state & 0x100);
                  mmb_held = (event.xmotion.state & 0x200);
                  rmb_held = (event.xmotion.state & 0x400);
                  mouse_x = event.xbutton.x;
                  mouse_y = state->win_h - event.xbutton.y - 1;

                  mouse_delta_x += mouse_x - prev_mouse_x;
                  mouse_delta_y += mouse_y - prev_mouse_y;
                } break;

                case FocusIn:
                {
                  has_focus = true;
                  // printf("FocusIn\n");

                  int device_count = 0;
                  int master_pointer_device_id = 2;
                  XIDeviceInfo* device_infos = XIQueryDevice(display, master_pointer_device_id, &device_count);
                  DEBUG_LOG("\nWindow got focus. Devices:\n");
                  for(i32 device_idx = 0;
                      device_idx < device_count;
                      ++device_idx)
                  {
                    XIDeviceInfo* device = &device_infos[device_idx];
                    DEBUG_LOG("  deviceid: %d\n", device->deviceid);
                    DEBUG_LOG("  name: %s\n", device->name);
                    xi_update_device_info(state, device->num_classes, device->classes);
                  }
                } break;

                case FocusOut:
                {
                  has_focus = false;
                } break;

                case ConfigureNotify:
                {
                  state->win_w = event.xconfigure.width;
                  state->win_h = event.xconfigure.height;

                  mouse_x = 0.5f * (r32)state->win_w;
                  mouse_y = 0.5f * (r32)state->win_h;
                } break;

                case Expose:
                {
                  dirty = true;
                } break;

                case DestroyNotify:
                {
                  quitting = true;
                } break;

                case MappingNotify:
                {
                  // printf(stderr, "Key mapping changed\n");
                  if(event.xmapping.request == MappingModifier ||
                      event.xmapping.request == MappingKeyboard)
                  {
                    XRefreshKeyboardMapping(&event.xmapping);
                  }
                } break;

                case SelectionNotify:
                {
                  if(event.xselection.property != None)
                  {
                    Atom type = 0;
                    int format = 0;
                    unsigned long item_count = 0;
                    unsigned long bytes_left = 0;
                    u8* data;
                    if(event.xselection.property != atom_mycliptarget)
                    {
                      printf("Paste: Got other target property!\n");
                    }
                    XGetWindowProperty(display, window, event.xselection.property,
                        0, 256,
                        false, AnyPropertyType, &type, &format,
                        &item_count, &bytes_left, &data);
                    if(type == atom_incr)
                    {
                      printf("Paste: INCR!\n");
                    }
                    printf("Paste: format: %d, count: %lu, bytes left: %lu, data: %s\n",
                        format, item_count, bytes_left, data);
                    // TODO: Parse URL-encoded file://... path and open it.
                    XFree(data);
                    XDeleteProperty(display, window, event.xselection.property);
                  }
                } break;

                case SelectionRequest:
                {
                  XSelectionRequestEvent* request = (XSelectionRequestEvent*)&event.xselectionrequest;

                  XSelectionEvent response = {0};
                  response.type = SelectionNotify;
                  response.requestor = request->requestor;
                  response.selection = request->selection;
                  response.target = request->target;
                  response.property = None;  // Denied.
                  response.time = request->time;

                  b32 respond_ok = false;

                  b32 any_marked = false;
                  for_count(i, state->total_img_count) { any_marked = any_marked || (state->img_entries[i].flags & IMG_FLAG_MARKED); }

                  if(request->property != None && clipboard_str.data != 0)
                  {
                    if(request->target == atom_targets)
                    {
                      Atom target_list[] = {
                        atom_targets,
                        atom_uri_list,
                        atom_utf8,
                      };
                      XChangeProperty(display, request->requestor, request->property,
                          XA_ATOM, 32, PropModeReplace, (u8*)target_list, array_count(target_list));

                      respond_ok = true;
                    }
                    else if(request->target == atom_uri_list)
                    {
                      for(i32 img_idx = 0;
                          img_idx < state->total_img_count;
                          ++img_idx)
                      {
                        char* path = 0;
                        if(any_marked)
                        {
                          if(!(state->img_entries[img_idx].flags & IMG_FLAG_MARKED)) { continue; }
                          path = (char*)state->img_entries[img_idx].path.data;
                        }
                        else
                        {
                          path = (char*)clipboard_str.data;
                        }
                        char* full_path = realpath(path, 0);

                        if(full_path)
                        {
                          // URI-encode the path.
                          u8 buf[4096];
                          u8* buf_end = buf + array_count(buf);
                          u8* buf_ptr = buf;

                          if(respond_ok)
                          {
                            *buf_ptr++ = '\n';
                          }

                          *buf_ptr++ = 'f';
                          *buf_ptr++ = 'i';
                          *buf_ptr++ = 'l';
                          *buf_ptr++ = 'e';
                          *buf_ptr++ = ':';
                          *buf_ptr++ = '/';
                          *buf_ptr++ = '/';

                          for(char* path_ptr = full_path;
                              *path_ptr && buf_ptr + 3 < buf_end;
                              ++path_ptr)
                          {
                            u8 c = *path_ptr;
                            if((c >= 'A' && c <= 'Z') ||
                                (c >= 'a' && c <= 'z') ||
                                (c >= '0' && c <= '9') ||
                                c == '-' || c == '_' || c == '.' || c == '~' || c == '/')
                            {
                              *buf_ptr++ = c;
                            }
                            else
                            {
                              char* hex = "0123456789ABCDEF";
                              *buf_ptr++ = '%';
                              *buf_ptr++ = hex[c >> 4];
                              *buf_ptr++ = hex[c & 0xF];
                            }
                          }

                          free(full_path);

                          XChangeProperty(display, request->requestor, request->property,
                              atom_uri_list, 8, respond_ok ? PropModeAppend : PropModeReplace,
                              buf, buf_ptr - buf);

                          respond_ok = true;
                        }

                        if(!any_marked) { break; }
                      }
                    }
                    else if(request->target == atom_utf8)
                    {
                      XChangeProperty(display, request->requestor, request->property,
                          atom_utf8, 8, PropModeReplace,
                          clipboard_str.data, clipboard_str.size);

                      respond_ok = true;
                    }
                    else
                    {
                      char* property_name = XGetAtomName(display, request->property);
                      char* target_name = XGetAtomName(display, request->target);
                      printf("Unhandled SelectionRequest target: %s, property: %s\n", target_name, property_name);
                      if(property_name) { XFree(property_name); }
                      if(target_name) { XFree(target_name); }
                    }
                  }

                  if(respond_ok)
                  {
                    response.property = request->property;
                  }

                  XSendEvent(display, request->requestor, True, NoEventMask, (XEvent*)&response);
                } break;

                default:
                {
                  // printf("Unhandled event type=%u\n", event.type);
                } break;
              }
            }

            // Handle mouse events.

            if(interaction_eq(current_interaction, mainview_interaction) &&
                !state->mouse_moved_since_dragging_start)
            {
              if(mouse_btn_went_up == 1)
              {
                state->viewing_filtered_img_idx += 1;
                scroll_thumbnail_into_view = true;
                dirty = true;
              }
              else if(mouse_btn_went_up == 2)
              {
                state->viewing_filtered_img_idx -= 1;
                scroll_thumbnail_into_view = true;
                dirty = true;
              }
            }

            if(mouse_delta_x != 0 || mouse_delta_y != 0)
            {
              state->mouse_moved_since_dragging_start = true;
            }

            if(!lmb_held && !mmb_held && !rmb_held)
            {
              zero_struct(current_interaction);
            }

            b32 mouse_on_sidebar_edge = false;
            b32 mouse_in_sidebar = false;
            b32 mouse_on_scrollbar = false;
            b32 mouse_on_info_panel_edge = false;
            b32 mouse_in_info_panel = false;
            if(!state->hide_sidebar
                && mouse_x >= effective_sidebar_width - get_scrollbar_width(state)
                && mouse_x < effective_sidebar_width)
            {
              mouse_on_scrollbar = true;
            }
            else if(!state->hide_sidebar
                && mouse_x >= effective_sidebar_width
                && mouse_x < effective_sidebar_width + 10)
            {
              mouse_on_sidebar_edge = true;
            }
            else if(!state->hide_sidebar && !mouse_on_sidebar_edge && mouse_x < effective_sidebar_width)
            {
              mouse_in_sidebar = true;
            }
            else if(state->show_info == 2 && absolute(mouse_x - (state->win_w - effective_info_panel_width)) <= 10)
            {
              mouse_on_info_panel_edge = true;
            }
            else if(state->show_info == 2 && mouse_x > state->win_w - effective_info_panel_width)
            {
              mouse_in_info_panel = true;
            }

            if(interaction_is_empty(current_interaction))
            {
              if(!has_focus)
              {
                zero_struct(hovered_interaction);
              }
              else if(mouse_on_scrollbar)
              {
                hovered_interaction = scrollbar_interaction;
                state->dragging_start_value2 = state->sidebar_scroll_rows;
              }
              else if(mouse_on_sidebar_edge)
              {
                hovered_interaction = sidebar_resize_interaction;
                state->dragging_start_value = effective_sidebar_width;
                state->dragging_start_value2 = state->sidebar_scroll_rows;
              }
              else if(mouse_in_sidebar)
              {
                hovered_interaction = sidebar_interaction;

                state->dragging_start_value = 0;
                img_entry_t* hovered_img = get_filtered_img(state, hovered_thumbnail_idx);
                if(hovered_img)
                {
                  state->dragging_start_value = (hovered_img->flags & IMG_FLAG_MARKED);
                }
              }
              else if(mouse_on_info_panel_edge)
              {
                hovered_interaction = info_panel_resize_interaction;
                state->dragging_start_value = effective_info_panel_width;
              }
              else if(mouse_in_info_panel)
              {
                zero_struct(hovered_interaction);
              }
              else
              {
                hovered_interaction = mainview_interaction;
              }

              if(mouse_btn_went_down)
              {
                current_interaction = hovered_interaction;

                state->dragging_start_x = mouse_x;
                state->dragging_start_y = mouse_y;
                state->mouse_moved_since_dragging_start = false;
              }
            }

            if(interaction_eq(current_interaction, sidebar_interaction) && lmb_held)
            {
              if(hovered_thumbnail_idx != -1)
              {
                b32 set_mark = !state->dragging_start_value;

                if(shift_held)
                {
                  i32 step = hovered_thumbnail_idx >= state->viewing_filtered_img_idx ? 1 : -1;
                  for(i32 idx = state->viewing_filtered_img_idx;
                      idx != hovered_thumbnail_idx;
                      idx += step)
                  {
                    set_or_unset_filtered_img_flag(state, idx, IMG_FLAG_MARKED, set_mark);
                  }
                  set_or_unset_filtered_img_flag(state, hovered_thumbnail_idx, IMG_FLAG_MARKED, set_mark);
                }
                else if(ctrl_held)
                {
                  set_or_unset_filtered_img_flag(state, hovered_thumbnail_idx, IMG_FLAG_MARKED, set_mark);
                }

                state->viewing_filtered_img_idx = hovered_thumbnail_idx;
                // Do not scroll the newly selected thumbnail into view!
                // It would mess with the extra-row padding.
              }
              dirty = true;
            }
            else if(interaction_eq(current_interaction, scrollbar_interaction))
            {
              // TODO: Make this match the exact logic of the scrollbar visual.
              i32 total_thumbnail_rows = (state->filtered_img_count + state->thumbnail_columns - 1) / state->thumbnail_columns + 1;
              if(mmb_held)
              {
                state->sidebar_scroll_rows = (state->win_h - mouse_y) * total_thumbnail_rows / state->win_h;
              }
              else if(lmb_held)
              {
                state->sidebar_scroll_rows -= mouse_delta_y * total_thumbnail_rows / state->win_h;
              }
              clamp_sidebar_scroll_rows(state);
              dirty = true;
            }
            else if(interaction_eq(current_interaction, sidebar_resize_interaction))
            {
              if(lmb_held)
              {
                state->sidebar_width = state->dragging_start_value + (i32)(mouse_x - state->dragging_start_x);
                state->sidebar_scroll_rows = state->dragging_start_value2;
                clamp_sidebar_scroll_rows(state);
                dirty = true;
              }
            }
            else if(interaction_eq(current_interaction, info_panel_resize_interaction))
            {
              if(lmb_held)
              {
                state->info_panel_width = state->dragging_start_value - (i32)(mouse_x - state->dragging_start_x);
                state->info_panel_width = max(1, state->info_panel_width);
                dirty = true;
              }
            }
            else if(mouse_delta_x || mouse_delta_y || scroll_x || scroll_y || scroll_y_ticks || mouse_btn_went_down)
            {
              r32 exp_zoom_before = exp2f(zoom);
              r32 offset_per_scroll = offset_scroll_scale / exp_zoom_before;
              r32 zoom_delta = 0;

              if(alt_held)
              {
                if(scroll_y_ticks)
                {
                  state->viewing_filtered_img_idx -= scroll_y_ticks;
                  scroll_thumbnail_into_view = true;
                }
              }

              if(!alt_held)
              {
                if(mouse_in_sidebar || mouse_on_scrollbar)
                {
                  if(ctrl_held)
                  {
                    i32 prev_visible_idx = hovered_thumbnail_idx;
                    if(prev_visible_idx == -1)
                    {
                      prev_visible_idx = (state->sidebar_scroll_rows + 1) * state->thumbnail_columns;
                    }
                    r32 prev_visible_top_y = (prev_visible_idx / state->thumbnail_columns - state->sidebar_scroll_rows) * thumbnail_h;

                    state->thumbnail_columns -= scroll_y_ticks;
                    clamp_thumbnail_columns(state);
                    thumbnail_h = get_thumbnail_size(state);

                    r32 new_visible_top_y = (prev_visible_idx / state->thumbnail_columns - state->sidebar_scroll_rows) * thumbnail_h;
                    state->sidebar_scroll_rows += (new_visible_top_y - prev_visible_top_y) / thumbnail_h;
                    clamp_sidebar_scroll_rows(state);
                  }
                  else if(shift_held)
                  {
                  }
                  else
                  {
                    state->sidebar_scroll_rows -= scroll_y;
                    clamp_sidebar_scroll_rows(state);
                  }
                }
                else
                {
                  // if(ctrl_held)
                  if(1 || ctrl_held)
                  {
                    zoom_delta += zoom_scroll_scale * scroll_y;
                  }
                  else if(shift_held)
                  {
                    offset_x += offset_per_scroll * scroll_y;
                  }
                  else
                  {
                    offset_x -= offset_per_scroll * scroll_x;
                    offset_y -= offset_per_scroll * scroll_y;
                  }
                }
              }

              if(zoom_delta != 0.0f)
              {
                state->dragging_start_x = mouse_x;
                state->dragging_start_y = mouse_y;
              }

              if(interaction_eq(current_interaction, mainview_interaction))
              {
                if(ctrl_held || mmb_held)
                {
                  zoom_delta += 4.0f * mouse_delta_y / win_min_side;
                }
                else
                {
                  offset_x += mouse_delta_x / (exp_zoom_before * win_min_side);
                  offset_y += mouse_delta_y / (exp_zoom_before * win_min_side);

                  state->dragging_start_x = mouse_x;
                  state->dragging_start_y = mouse_y;
                }
              }

              if(zoom_delta != 0.0f)
              {
                zoom += zoom_delta;
                r32 exp_zoom_after = exp2f(zoom);

                r32 center_mouse_x = state->dragging_start_x - (image_region_x0 + 0.5f * image_region_w);
                r32 center_mouse_y = state->dragging_start_y - (image_region_y0 + 0.5f * image_region_h);

                offset_x += center_mouse_x / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
                offset_y += center_mouse_y / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
              }

              dirty = true;
            }

            prev_mouse_x = mouse_x;
            prev_mouse_y = mouse_y;
          }

          if(search_changed)
          {
            if(state->search_str.size == 0)
            {
              for_count(i, state->total_img_count) { state->filtered_img_idxs[i] = i; }
              state->filtered_img_count = state->total_img_count;
              state->viewing_filtered_img_idx = state->img_idx_viewed_before_search;
            }
            else
            {
              state->filtered_img_count = 0;
              state->viewing_filtered_img_idx = 0;

              str_t query = state->search_str;
              u8* query_end = query.data + query.size;

              i32 query_positive_word_count = 0;
              i32 query_negative_word_count = 0;
              str_t query_positive_words[256];
              str_t query_negative_words[256];
              // TODO: Turn a (str_t, b32) pair into a search-item structure?
              BITSET32(exclude_positive_words, array_count(query_positive_words)) = {0};
              BITSET32(exclude_negative_words, array_count(query_negative_words)) = {0};
              str_t query_model = {0};
              for(u8 *word_start = query.data, *word_end = query.data;
                  word_start < query_end && query_positive_word_count < array_count(query_positive_words);
                 )
              {
                // TODO: Consider "quoted strings" as one word.
                while(word_start < query_end &&
                    (*word_start == ' ' ||
                     *word_start == ','))
                {
                  ++word_start;
                }

                b32 exclude = false;
                if(*word_start == '-')
                {
                  exclude = true;
                  ++word_start;
                }

                word_end = word_start;

                u8* column_at = 0;
                while(word_end < query_end &&
                    !(*word_end == ' ' ||
                      *word_end == ','))
                {
                  if(!column_at && *word_end == ':')
                  {
                    column_at = word_end;
                  }

                  ++word_end;
                }

                str_t pre_column = {0};
                str_t post_column = {0};
                if(column_at)
                {
                  pre_column = str_from_span(word_start, column_at);
                  post_column = str_from_span(column_at + 1, word_end);
                }

                if(str_eq(pre_column, str("m")))
                {
                  query_model = post_column;
                }
                else if(str_eq(pre_column, str("n")))
                {
                  if(post_column.size)
                  {
                    if(query_negative_word_count < array_count(query_negative_words))
                    {
                      if(exclude) { bitset32_set(exclude_negative_words, query_negative_word_count); }
                      query_negative_words[query_negative_word_count] = post_column;
                      ++query_negative_word_count;
                    }
                  }
                }
                else if(word_end > word_start)
                {
                  if(query_positive_word_count < array_count(query_positive_words))
                  {
                    if(exclude) { bitset32_set(exclude_positive_words, query_positive_word_count); }
                    query_positive_words[query_positive_word_count] = str_from_span(word_start, word_end);
                    ++query_positive_word_count;
                  }
                }

                word_start = word_end;
              }

#if 0
              printf("\nwords:\n");
              for(i32 word_idx = 0;
                  word_idx < query_positive_word_count;
                  ++word_idx)
              {
                printf("  \"%.*s\"\n", PF_STR(query_positive_words[word_idx]));
              }
#endif

              for_count(img_idx, state->total_img_count)
              {
                b32 overall_match = true;
                img_entry_t* img = &state->img_entries[img_idx];

                if(query_model.size > 0)
                {
                  b32 model_matches = false;
                  for(i64 offset = 0;
                      offset <= (i64)img->model.size - (i64)query_model.size && !model_matches;
                      ++offset)
                  {
                    str_t model_substr = { img->model.data + offset, query_model.size };
                    if(str_eq_ignoring_case(model_substr, query_model))
                    {
                      model_matches = true;
                    }
                  }

                  overall_match = overall_match && model_matches;
                }

                BITSET32(positive_words_matched, array_count(query_positive_words)) = {0};
                BITSET32(negative_words_matched, array_count(query_negative_words)) = {0};

                for(i64 offset = 0;
                    offset < (i64)img->positive_prompt.size && query_positive_word_count > 0 && overall_match;
                    ++offset)
                {
                  for(i32 word_idx = 0;
                      word_idx < query_positive_word_count;
                      ++word_idx)
                  {
                    str_t query_positive_word = query_positive_words[word_idx];

                    if(!bitset32_get(positive_words_matched, word_idx) &&
                        img->positive_prompt.size >= query_positive_word.size + offset)
                    {
                      // TODO: If multiple query word prefixes match, pick the longest one eagerly.
                      str_t prompt_substr = { img->positive_prompt.data + offset, query_positive_word.size };
                      if(str_eq_ignoring_case(prompt_substr, query_positive_word))
                      {
                        // TODO: Think about whether to consider already-matched words
                        //       earlier in the prompt for exclusion.
                        if(bitset32_get(exclude_positive_words, word_idx))
                        {
                          overall_match = false;
                        }
                        else
                        {
                          bitset32_set(positive_words_matched, word_idx);
                        }
                        break;
                      }
                    }
                  }
                }

                for(i64 offset = 0;
                    offset < (i64)img->negative_prompt.size && query_negative_word_count > 0 && overall_match;
                    ++offset)
                {
                  for(i32 word_idx = 0;
                      word_idx < query_negative_word_count;
                      ++word_idx)
                  {
                    str_t query_negative_word = query_negative_words[word_idx];

                    if(!bitset32_get(negative_words_matched, word_idx) &&
                        img->negative_prompt.size >= query_negative_word.size + offset)
                    {
                      // TODO: If multiple query word prefixes match, pick the longest one eagerly.
                      str_t prompt_substr = { img->negative_prompt.data + offset, query_negative_word.size };
                      if(str_eq_ignoring_case(prompt_substr, query_negative_word))
                      {
                        if(bitset32_get(exclude_negative_words, word_idx))
                        {
                          overall_match = false;
                        }
                        else
                        {
                          bitset32_set(negative_words_matched, word_idx);
                        }
                        break;
                      }
                    }
                  }
                }

                for_count(word_idx, query_positive_word_count)
                {
                  if(!bitset32_get(exclude_positive_words, word_idx))
                  {
                    overall_match = overall_match && bitset32_get(positive_words_matched, word_idx);
                  }
                }
                for_count(word_idx, query_negative_word_count)
                {
                  if(!bitset32_get(exclude_negative_words, word_idx))
                  {
                    overall_match = overall_match && bitset32_get(negative_words_matched, word_idx);
                  }
                }
                if(overall_match)
                {
                  if(state->img_idx_viewed_before_search >= img_idx)
                  {
                    state->viewing_filtered_img_idx = state->filtered_img_count;
                  }

                  state->filtered_img_idxs[state->filtered_img_count++] = img_idx;
                }
              }
            }

            dirty = true;
            scroll_thumbnail_into_view = true;
          }

          if(dirty)
          {
            // Increase this to play out animations and such after events stop.
            // Might be a better idea to do a diff on the presentation states though.
            dirty_frames = 1;
          }

          if(dirty_frames > 0)
          {
            --dirty_frames;

            r32 thumbnail_w = get_thumbnail_size(state);
            r32 thumbnail_h = thumbnail_w;

#if 1
            state->viewing_filtered_img_idx =
              clamp(0, state->filtered_img_count - 1, state->viewing_filtered_img_idx);
#else
            state->viewing_filtered_img_idx = i32_wrap_upto(state->viewing_filtered_img_idx, state->filtered_img_count);
#endif
            i32 viewing_img_idx = -1;
            img_entry_t dummy_img = {0};
            img_entry_t* img = &dummy_img;
            if(state->filtered_img_count > 0)
            {
              viewing_img_idx = state->filtered_img_idxs[state->viewing_filtered_img_idx];
              img = &state->img_entries[viewing_img_idx];
            }

            if(last_viewing_img_idx != viewing_img_idx && img->load_state == LOAD_STATE_LOADED_INTO_RAM)
            {
              char txt[256];
              txt[0] = 0;
              i32 txt_len = snprintf(txt, sizeof(txt), "%s [%d/%d] %dx%d %s",
                  PROGRAM_NAME,
                  viewing_img_idx + 1, state->total_img_count,
                  img->w, img->h,
                  img->path.data);
              set_title(display, window, (u8*)txt, txt_len);

#if 0
              // if(state->show_info)
              {
#define P(x) printf("  " #x ": %.*s\n", (int)img->x.size, img->x.data);
                puts("");
                puts(txt);
                // P(generation_parameters);
                P(positive_prompt);
                P(negative_prompt);
                P(seed);
                P(batch_size);
                P(model);
                P(sampler);
                P(sampling_steps);
                P(cfg);
#undef P
              }
#endif

              last_viewing_img_idx = viewing_img_idx;
            }

            if(scroll_thumbnail_into_view)
            {
              scroll_thumbnail_into_view = false;
              i32 thumbnail_row = state->viewing_filtered_img_idx / state->thumbnail_columns;
#if 0
              state->sidebar_scroll_rows = thumbnail_row - 0.5f * state->win_h / thumbnail_h + 0.5f;
#else
              // i32 extra_rows = (state->win_h >= 2 * thumbnail_h) ? 1 : 0;
              i32 extra_rows = (i32)(0.3f * state->win_h / thumbnail_h);

              state->sidebar_scroll_rows = clamp(
                  max(0, thumbnail_row + 1 - state->win_h / thumbnail_h + extra_rows),
                  thumbnail_row - extra_rows,
                  state->sidebar_scroll_rows);
#endif

              clamp_sidebar_scroll_rows(state);
            }

            i32 first_visible_row = (i32)state->sidebar_scroll_rows;
            i32 one_past_last_visible_row = (i32)(state->sidebar_scroll_rows + state->win_h / thumbnail_h + 1);
            i32 first_visible_thumbnail_idx = max(0, first_visible_row * state->thumbnail_columns);
            i32 last_visible_thumbnail_idx = min(state->filtered_img_count, one_past_last_visible_row * state->thumbnail_columns) - 1;
            if(0
                || state->viewing_filtered_img_idx != shared_loader_data->viewing_filtered_img_idx
                || first_visible_thumbnail_idx != shared_loader_data->first_visible_thumbnail_idx
                || last_visible_thumbnail_idx != shared_loader_data->last_visible_thumbnail_idx
                || state->filtered_img_count != shared_loader_data->filtered_img_count
                || reloaded_paths
              )
            {
              shared_loader_data->viewing_filtered_img_idx = state->viewing_filtered_img_idx;
              shared_loader_data->first_visible_thumbnail_idx = first_visible_thumbnail_idx;
              shared_loader_data->last_visible_thumbnail_idx = last_visible_thumbnail_idx;
              shared_loader_data->filtered_img_count = state->filtered_img_count;
              for_count(i, loader_count) { sem_post(&loader_semaphores[i]); }
            }

#if 0
            if(game_memory.grab_mouse && !pointer_grabbed)
            {
              XGrabPointer(display, window,
                  true, 0, GrabModeAsync, GrabModeAsync, window, None, CurrentTime);
              pointer_grabbed = true;
            }
            else if(!game_memory.grab_mouse && pointer_grabbed)
            {
              XUngrabPointer(display, CurrentTime);
              pointer_grabbed = false;
            }

            if(should_hide_mouse && !cursor_hidden)
            {
              XDefineCursor(display, window, empty_cursor);
              cursor_hidden = true;
            }
            else if(!should_hide_mouse && cursor_hidden)
            {
              XUndefineCursor(display, window);
              cursor_hidden = false;
            }
#endif

            glViewport(0, 0, state->win_w, state->win_h);
            glDisable(GL_SCISSOR_TEST);
            {
              r32 bg_gray = bright_bg ? 0.9f : 0.1f;
              glClearColor(bg_gray, bg_gray, bg_gray, 1.0f);
            }
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);

            if(state->win_w != 0 && state->win_h != 0)
            {
              glMatrixMode(GL_PROJECTION);
              r32 matrix[16] = { // Column-major!
                2.0f / state->win_w, 0.0f, 0.0f, 0.0f,
                0.0f, 2.0f / state->win_h, 0.0f, 0.0f,
                0.0f, 0.0f, 1.0f, 0.0f,
                -1.0f, -1.0f, 0.0f, 1.0f,
              };
              glLoadMatrixf(matrix);
            }

            b32 still_loading = false;

            if(!state->hide_sidebar)
            {
              glScissor(0, 0, effective_sidebar_width, state->win_h);

              glDisable(GL_BLEND);
              glDisable(GL_TEXTURE_2D);

              glBegin(GL_QUADS);

              // Scrollbar.
              {
                r32 scrollbar_edge_gray = 0.5f;
                if(interaction_eq(hovered_interaction, scrollbar_interaction))
                {
                  scrollbar_edge_gray = bright_bg ? 0.3f : 0.7f;
                }
                glColor3f(scrollbar_edge_gray, scrollbar_edge_gray, scrollbar_edge_gray);

                i32 scrollbar_width = get_scrollbar_width(state);
                i32 total_thumbnail_rows = (state->filtered_img_count + state->thumbnail_columns - 1) / state->thumbnail_columns + 1;
                r32 scrollbar_top_ratio = state->sidebar_scroll_rows / (r32)total_thumbnail_rows;
                r32 scrollbar_bottom_ratio = (state->sidebar_scroll_rows + state->win_h / thumbnail_h) / (r32)total_thumbnail_rows;
                i32 scrollbar_yrange = max(2, (i32)(state->win_h * (scrollbar_bottom_ratio - scrollbar_top_ratio) + 0.5f));
                i32 scrollbar_y1 = (i32)(state->win_h * (1 - scrollbar_top_ratio) + 0.5f);
                i32 scrollbar_y0 = scrollbar_y1 - scrollbar_yrange;

                glVertex2f(effective_sidebar_width - scrollbar_width, scrollbar_y0);
                glVertex2f(effective_sidebar_width - 2, scrollbar_y0);
                glVertex2f(effective_sidebar_width - 2, scrollbar_y1);
                glVertex2f(effective_sidebar_width - scrollbar_width, scrollbar_y1);
              }

              // Resizing bar.
              r32 sidebar_edge_gray = 0.0f;
              if(interaction_eq(hovered_interaction, sidebar_resize_interaction))
              {
                sidebar_edge_gray = bright_bg ? 0.0f : 1.0f;
              }
              else
              {
                sidebar_edge_gray = bright_bg ? 0.4f : 0.6f;
              }

              glColor3f(sidebar_edge_gray, sidebar_edge_gray, sidebar_edge_gray);
              glVertex2f(effective_sidebar_width - 2, 0);
              glVertex2f(effective_sidebar_width - 1, 0);
              glVertex2f(effective_sidebar_width - 1, state->win_h);
              glVertex2f(effective_sidebar_width - 2, state->win_h);

              glEnd();

              glScissor(0, 0, effective_sidebar_width - 2, state->win_h);

              if(state->alpha_blend)
              {
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glEnable(GL_BLEND);
              }
              else
              {
                glDisable(GL_BLEND);
              }

              glEnable(GL_TEXTURE_2D);
              glColor3f(1.0f, 1.0f, 1.0f);
              hovered_thumbnail_idx = -1;
              for(i32 filtered_idx = first_visible_thumbnail_idx;
                  filtered_idx <= last_visible_thumbnail_idx;
                  ++filtered_idx)
              {
                img_entry_t* thumb = get_filtered_img(state, filtered_idx);
                still_loading |= upload_img_texture(state, thumb);

                i32 sidebar_col = filtered_idx % state->thumbnail_columns;
                i32 sidebar_row = filtered_idx / state->thumbnail_columns;

                r32 box_x0 = sidebar_col * thumbnail_w;
                r32 box_y1 = state->win_h - ((r32)sidebar_row - state->sidebar_scroll_rows) * thumbnail_h;
                r32 box_x1 = box_x0 + thumbnail_w;
                r32 box_y0 = box_y1 - thumbnail_h;

                if(hovered_thumbnail_idx == -1 &&
                    interaction_eq(hovered_interaction, sidebar_interaction) &&
                    max(0, prev_mouse_x) >= box_x0 &&
                    (prev_mouse_x < box_x1 || sidebar_col == state->thumbnail_columns - 1) &&
                    prev_mouse_y >= box_y0 &&
                    prev_mouse_y < box_y1
                  )
                {
                  hovered_thumbnail_idx = filtered_idx;
                }

                if(filtered_idx == state->viewing_filtered_img_idx || filtered_idx == hovered_thumbnail_idx)
                {
                  glDisable(GL_TEXTURE_2D);

                  r32 gray = (filtered_idx == state->viewing_filtered_img_idx) ? (bright_bg ? 0.3f : 0.7f) : 0.5f;
                  glColor3f(gray, gray, gray);

                  glBegin(GL_QUADS);
                  glVertex2f(box_x0, box_y0);
                  glVertex2f(box_x1, box_y0);
                  glVertex2f(box_x1, box_y1);
                  glVertex2f(box_x0, box_y1);
                  glEnd();
                }

                GLuint texture_id = thumb->texture_id;
                r32 tex_w = thumb->w;
                r32 tex_h = thumb->h;
                if(thumb->load_state == LOAD_STATE_LOAD_FAILED)
                {
                  texture_id = state->test_texture_id;
                  tex_w = test_texture_w;
                  tex_h = test_texture_h;
                }

                if(texture_id)
                {
                  glEnable(GL_TEXTURE_2D);
                  glBindTexture(GL_TEXTURE_2D, texture_id);
                  glColor3f(1.0f, 1.0f, 1.0f);

                  r32 mag = 1.0f;
                  if(thumbnail_w != 0 && thumbnail_h != 0)
                  {
                    mag = min((r32)thumbnail_w / (r32)tex_w, (r32)thumbnail_h / (r32)tex_h);
                  }
                  mag *= 0.9f;

                  r32 x0 = box_x0;
                  r32 y1 = box_y1;

                  x0 += 0.5f * (thumbnail_w - mag * tex_w);
                  y1 -= 0.5f * (thumbnail_h - mag * tex_h);

                  r32 x1 = x0 + mag * tex_w;
                  r32 y0 = y1 - mag * tex_h;

                  r32 u0 = 0;
                  r32 v0 = 0;
                  r32 u1 = 1;
                  r32 v1 = 1;

                  glBegin(GL_QUADS);
                  glTexCoord2f(u0, v1); glVertex2f(x0, y0);
                  glTexCoord2f(u1, v1); glVertex2f(x1, y0);
                  glTexCoord2f(u1, v0); glVertex2f(x1, y1);
                  glTexCoord2f(u0, v0); glVertex2f(x0, y1);
                  glEnd();
                }

                if(thumb->flags & IMG_FLAG_MARKED)
                {
                  glDisable(GL_TEXTURE_2D);
                  glLineWidth(2.0f);
                  glColor3f(0.0f, 0.0f, 0.0f);
                  glBegin(GL_LINE_STRIP);
                  glVertex2f(lerp(box_x0, box_x1, 0.1f) + 2, lerp(box_y0, box_y1, 0.7f) - 2);
                  glVertex2f(lerp(box_x0, box_x1, 0.1f) + 2, lerp(box_y0, box_y1, 0.9f) - 2);
                  glVertex2f(lerp(box_x0, box_x1, 0.2f) + 2, lerp(box_y0, box_y1, 0.8f) - 2);
                  glVertex2f(lerp(box_x0, box_x1, 0.3f) + 2, lerp(box_y0, box_y1, 0.9f) - 2);
                  glVertex2f(lerp(box_x0, box_x1, 0.3f) + 2, lerp(box_y0, box_y1, 0.7f) - 2);
                  glEnd();
                  glColor3f(0.2f, 1.0f, 0.2f);
                  glBegin(GL_LINE_STRIP);
                  glVertex2f(lerp(box_x0, box_x1, 0.1f),     lerp(box_y0, box_y1, 0.7f));
                  glVertex2f(lerp(box_x0, box_x1, 0.1f),     lerp(box_y0, box_y1, 0.9f));
                  glVertex2f(lerp(box_x0, box_x1, 0.2f),     lerp(box_y0, box_y1, 0.8f));
                  glVertex2f(lerp(box_x0, box_x1, 0.3f),     lerp(box_y0, box_y1, 0.9f));
                  glVertex2f(lerp(box_x0, box_x1, 0.3f),     lerp(box_y0, box_y1, 0.7f));
                  glEnd();
                }
              }
            }

            still_loading |= upload_img_texture(state, img);

            GLuint texture_id = img->texture_id;
            r32 tex_w = img->w;
            r32 tex_h = img->h;
            if(img->load_state == LOAD_STATE_LOAD_FAILED)
            {
              texture_id = state->test_texture_id;
              tex_w = test_texture_w;
              tex_h = test_texture_h;
            }

            // if(texture_id)
            {
              if(state->alpha_blend)
              {
                glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                glEnable(GL_BLEND);
              }
              else
              {
                glDisable(GL_BLEND);
              }

#if 0
              // FONT TEST
              texture_id = state->font_texture_id;
              tex_w = state->font_texture_w;
              tex_h = state->font_texture_h;
              glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
#endif

              glBindTexture(GL_TEXTURE_2D, texture_id);

              r32 u0 = 0.0f;
              r32 v0 = 0.0f;
              r32 u1 = 1.0f;
              r32 v1 = 1.0f;

              glScissor(image_region_x0, image_region_y0, image_region_w, image_region_h);

              r32 mag = 1.0f;
              if(!state->zoom_from_original_size && tex_w != 0 && tex_h != 0)
              {
                mag = min((r32)image_region_w / (r32)tex_w, (r32)image_region_h / (r32)tex_h);
              }
              r32 exp_zoom = exp2f(zoom);
              mag *= exp_zoom;

              if(absolute(mag - (i32)(mag + 0.5f)) <= 1e-3f)
              {
                mag = (i32)(mag + 0.5f);
              }

              r32 x0 = 0.5f * (image_region_w - mag * tex_w) + image_region_x0;
              r32 y0 = 0.5f * (image_region_h - mag * tex_h) + image_region_y0;

              x0 += win_min_side * exp_zoom * offset_x;
              y0 += win_min_side * exp_zoom * offset_y;

              if(mag == (i32)mag)
              {
                // Avoid interpolating pixels when viewing them 1:1.
                x0 = (r32)(i32)(x0 + 0.5f);
                y0 = (r32)(i32)(y0 + 0.5f);
              }

              r32 x1 = x0 + mag * tex_w;
              r32 y1 = y0 + mag * tex_h;

              if(border_sampling)
              {
                r32 margin = max(1.0f, mag);

                u0 -= margin / (r32)(mag * tex_w);
                v0 -= margin / (r32)(mag * tex_h);
                u1 += margin / (r32)(mag * tex_w);
                v1 += margin / (r32)(mag * tex_h);

                x0 -= margin;
                y0 -= margin;
                x1 += margin;
                y1 += margin;
              }

              glEnable(GL_TEXTURE_2D);
              glColor3f(1.0f, 1.0f, 1.0f);
              glBegin(GL_QUADS);
              glTexCoord2f(u0, v1); glVertex2f(x0, y0);
              glTexCoord2f(u1, v1); glVertex2f(x1, y0);
              glTexCoord2f(u1, v0); glVertex2f(x1, y1);
              glTexCoord2f(u0, v0); glVertex2f(x0, y1);
              glEnd();
            }

            r32 text_gray = bright_bg ? 0 : 1;
            r32 label_gray = bright_bg ? 0.3f : 0.7f;

            if(state->show_info == 1)
            {
              glScissor(image_region_x0, 0, state->win_w - image_region_x0, info_height);
              // glScissor(0, 0, state->win_w, state->win_h);
              glColor3f(text_gray, text_gray, text_gray);

              r32 x = image_region_x0 + 0.2f * fs;
              r32 y = fs * (state->font_descent + 0.1f);
              str_t str = img->positive_prompt;
              draw_str(state, 0, 1, fs, x, y, str);
            }
            if(state->show_info == 2)
            {
              glScissor(
                  image_region_x0 + image_region_w, image_region_y0,
                  effective_info_panel_width, state->win_h);
              // glScissor(0, 0, state->win_w, state->win_h);

              glBindTexture(GL_TEXTURE_2D, 0);
              glDisable(GL_BLEND);
              r32 edge_gray = 0.0f;
              if(interaction_eq(hovered_interaction, info_panel_resize_interaction))
              {
                edge_gray = bright_bg ? 0.0f : 1.0f;
              }
              else
              {
                edge_gray = bright_bg ? 0.4f : 0.6f;
              }
              glColor3f(edge_gray, edge_gray, edge_gray);
              glBegin(GL_QUADS);
              glVertex2f(state->win_w - effective_info_panel_width,     0);
              glVertex2f(state->win_w - effective_info_panel_width + 1, 0);
              glVertex2f(state->win_w - effective_info_panel_width + 1, state->win_h);
              glVertex2f(state->win_w - effective_info_panel_width,     state->win_h);
              glEnd();

              r32 x0 = state->win_w - effective_info_panel_width + 0.5f * fs;
              r32 x1 = state->win_w - 0.2f * fs;
              r32 y1 = state->win_h - fs * (state->font_ascent + 0.3f);
              r32 x_indented = x0 + fs;

              r32 x = x0;
              r32 y = y1 + fs;

#if 0
              y -= fs;
              x = x0;
              glColor3f(label_gray, label_gray, label_gray);
              x += draw_str(state, 0, 1, fs, x, y, str("Text wrap test: "));
              glColor3f(text_gray, text_gray, text_gray);
              draw_wrapped_text(state, 0, fs, x_indented, x1, &x, &y,
                  str("\n ble afawpeij afweo a fafw pwfW\npfojawpovijpvij asdiopfj\n afjiop awiof pioawefj pioawjef\n"));
#endif

              u8 tmp[256];
              str_t tmp_str = { tmp };

#define SHOW_LABEL_VALUE(label, value) \
              if(label[0] == 0 || value.size > 0) \
              { \
                y -= fs; \
                x = x0; \
                glColor3f(label_gray, label_gray, label_gray); \
                x += draw_str(state, 0, 1, fs, x, y, str(label)); \
                glColor3f(text_gray, text_gray, text_gray); \
                draw_wrapped_text(state, 0, fs, x_indented, x1, &x, &y, value); \
              }

              tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%d/%d of %d total",
                  state->viewing_filtered_img_idx + 1, state->filtered_img_count, state->total_img_count);
              SHOW_LABEL_VALUE("", tmp_str);
              SHOW_LABEL_VALUE("", str(""));
              SHOW_LABEL_VALUE("File: ", img->path);
              if(img->w || img->h)
              {
                tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%dx%d", img->w, img->h);
              }
              SHOW_LABEL_VALUE("Resolution: ", tmp_str);
              SHOW_LABEL_VALUE("", str(""));
              SHOW_LABEL_VALUE("Model: ", img->model);
              SHOW_LABEL_VALUE("Sampler: ", img->sampler);
              SHOW_LABEL_VALUE("Sampling steps: ", img->sampling_steps);
              SHOW_LABEL_VALUE("CFG: ", img->cfg);
              SHOW_LABEL_VALUE("Batch size: ", img->batch_size);
              SHOW_LABEL_VALUE("Seed: ", img->seed);
              SHOW_LABEL_VALUE("Positive prompt: ", img->positive_prompt);
              SHOW_LABEL_VALUE("Negative prompt: ", img->negative_prompt);
#undef SHOW_LABEL_VALUE
            }

            if(state->searching)
            {
              glScissor(0, 0, state->win_w, state->win_h);
              r32 x0 = image_region_x0 + 0.5f * fs;
              r32 x1 = state->win_w - 0.6f * fs;
              r32 y1 = state->win_h - 1.5f * fs;
              r32 x_indented = x0 + fs;
              for(i32 pass = 1;
                  pass <= 2;
                  ++pass)
              {
                draw_str_flags_t flags = (pass == 1) ? DRAW_STR_MEASURE_ONLY : 0;
                r32 x = x0;
                r32 y = y1;

                glColor3f(label_gray, label_gray, label_gray);
                x += draw_str(state, flags, 1, fs, x, y, str("Search: "));

                i64 selection_min = min(state->selection_start, state->selection_end);
                i64 selection_max = max(state->selection_start, state->selection_end);
                str_t str_before_selection = { state->search_str.data, selection_min };
                str_t str_in_selection = str_from_span(state->search_str.data + selection_min,
                    state->search_str.data + selection_max);
                str_t str_after_selection = str_from_span(state->search_str.data + selection_max,
                    state->search_str.data + state->search_str.size);

                glColor3f(text_gray, text_gray, text_gray);
                draw_wrapped_text(state, flags, fs, x_indented, x1, &x, &y, str_before_selection);
                if(state->selection_end < state->selection_start)
                {
                  x += draw_str(state, flags, 1, fs, x, y, str("|"));
                }
                glColor3f(0, 1, 0);
                draw_wrapped_text(state, flags, fs, x_indented, x1, &x, &y, str_in_selection);
                glColor3f(text_gray, text_gray, text_gray);
                if(state->selection_end >= state->selection_start)
                {
                  x += draw_str(state, flags, 1, fs, x, y, str("|"));
                }
                draw_wrapped_text(state, flags, fs, x_indented, x1, &x, &y, str_after_selection);

                if(pass == 1)
                {
                  // Background rectangle.
                  r32 x_max = (y == y1) ? x : x1;
                  glColor3f(1 - text_gray, 1 - text_gray, 1 - text_gray);
                  glBindTexture(GL_TEXTURE_2D, 0);
                  glDisable(GL_BLEND);
                  glBegin(GL_QUADS);
                  glVertex2f(x0 - 0.3f * fs, y - fs * (state->font_descent + 0.2f));
                  glVertex2f(x_max + 0.3f * fs, y - fs * (state->font_descent + 0.2f));
                  glVertex2f(x_max + 0.3f * fs, y1 + fs * (state->font_ascent + 0.2f));
                  glVertex2f(x0 - 0.3f * fs, y1 + fs * (state->font_ascent + 0.2f));
                  glEnd();

                  glColor3f(text_gray, text_gray, text_gray);
                }
              }
            }

#if 0
            // https://www.khronos.org/opengl/wiki/Sync_Object#Synchronization
            // This doesn't seem to help; see glFinish below.
            GLsync fence = glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
            if(!fence) { printf("Could not create fence.\n"); }
            // glFlush();
            printf("%X\n", glClientWaitSync(fence, GL_SYNC_FLUSH_COMMANDS_BIT, 1000000000));
#endif

            glXSwapBuffers(display, glx_window);

#if 1
            if(state->vsync)
            {
              // glFinish seems to reduce lag on HP laptop.
              // On desktop, it doesn't help, and brings CPU usage to 100% :/
              // However, adding a sleep afterwards, does help on desktop!
              // Still more CPU use than without glFinish, though.
              // TODO: Wait for as long as necessary for slightly under the screen refresh rate.
              // TODO: Only do this if GLX_NV_delay_before_swap is not available.

              glFinish();
              usleep(10000);
            }
#endif

            if(still_loading)
            {
              ++dirty_frames;
            }
          }
          else
          {
            // Wait for next event.
            // See: https://nrk.neocities.org/articles/x11-timeout-with-xsyncalarm
            // printf("Waiting for next event...\n");
#if 1
            struct pollfd poll_fds[] = {
              { .fd = ConnectionNumber(display), .events = POLLIN },
              { .fd = state->inotify_fd, .events = POLLIN },
            };
            nfds_t poll_fd_count = 1;
            if(state->inotify_fd != -1)
            {
              poll_fd_count = 2;
            }
            // poll(poll_fds, poll_fd_count, 1000);
            poll(poll_fds, poll_fd_count, -1);
#else
            XEvent dummy;
            XPeekEvent(display, &dummy);
#endif
            // printf("Continuing!\n");
          }

          u64 nsecs_now = get_nanoseconds();
          i64 nsecs = nsecs_now - nsecs_last_frame;
          nsecs_last_frame = nsecs_now;
#if 1
          if(!state->vsync || show_fps)
          {
            ++frames_since_last_print;
            nsecs_min = min(nsecs_min, nsecs);
            nsecs_max = max(nsecs_max, nsecs);
            r32 secs_since_last_print = 1e-9f * (r32)(nsecs_now - nsecs_last_print);
            if(secs_since_last_print >= 1.0f)
            {
              printf("avg FPS: %.1f [%.1f - %.1f]\n",
                  (r32)frames_since_last_print / secs_since_last_print,
                  1e9f / (r32)nsecs_max,
                  1e9f / (r32)nsecs_min);
              frames_since_last_print = 0;
              nsecs_last_print = nsecs_now;
              nsecs_min = I64_MAX;
              nsecs_max = I64_MIN;
            }
          }
#endif

          // time += 1.0f / 60.0f;
          time += 1e-9f * nsecs;
          if(time >= 1000) { time -= 1000; }
        }
      }
      else
      {
        fprintf(stderr, "No GLX configs available.\n");
      }
    }
    else
    {
      fprintf(stderr, "Could not query GLX version.\n");
    }
  }
  else
  {
    fprintf(stderr, "Could not open X11 display.\n");
  }
}
