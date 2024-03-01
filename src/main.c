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
#if !RELEASE
#define DEBUG_LOG(...) if(debug_out) { fprintf(debug_out, __VA_ARGS__); }
#else
#define DEBUG_LOG(...)
#endif

// static void (*glXSwapIntervalEXT)(Display *dpy, GLXDrawable drawable, int interval);

extern void* _binary_data_DejaVuSans_ttf_start;
// extern void* _binary_data_DejaVuSans_ttf_end;
// extern void* _binary_data_DejaVuSans_ttf_size;

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

enum
{
  LOAD_STATE_UNLOADED = 0,
  LOAD_STATE_LOADING,
  LOAD_STATE_LOADED_INTO_RAM,
};
typedef u32 load_state_t;

typedef struct
{
  i32 entry_idx;

  u32 load_generation;

  i32 w;
  i32 h;
  u8* pixels;
  i64 bytes_used;

  volatile load_state_t load_state;
} loaded_img_t;

enum
{
  IMG_FLAG_UNUSED         = (1 << 0),  // Set if a file got deleted at a refresh.
  IMG_FLAG_FAILED_TO_LOAD = (1 << 1),
  IMG_FLAG_MARKED         = (1 << 2),
  IMG_FLAG_FILTERED       = (1 << 3),  // Only temporary, used during file refresh.
};
typedef u32 img_flags_t;

enum
{
  IMG_STR_GENERATION_PARAMETERS,
  IMG_STR_POSITIVE_PROMPT,
  IMG_STR_NEGATIVE_PROMPT,
  IMG_STR_SEED,
  IMG_STR_BATCH_SIZE,
  IMG_STR_MODEL,
  IMG_STR_SAMPLER,
  IMG_STR_SAMPLING_STEPS,
  IMG_STR_CFG,
  IMG_STR_SCORE,

  IMG_STR_COUNT,
};
typedef u32 img_str_t;

enum
{
  PARSED_R32_SAMPLING_STEPS,
  PARSED_R32_CFG,
  PARSED_R32_SCORE,

  PARSED_R32_COUNT,
};
typedef u32 parsed_r32_t;

typedef struct img_entry_t
{
  str_t path;
  struct timespec modified_at_time;
  u64 filesize;

  u32 metadata_generation;
  str_t file_header_data;
  str_t parameter_strings[IMG_STR_COUNT];
  r32 parsed_r32s[PARSED_R32_COUNT];

  img_flags_t flags;
  i32 w;
  i32 h;
  u8* pixels;
  u32 load_generation;
  GLuint texture_id;
  i64 bytes_used;
  u32 random_number;

  i32 thumbnail_column;
  r32 thumbnail_y;
  i32 thumbnail_group;

  struct img_entry_t* lru_prev;
  struct img_entry_t* lru_next;

  volatile load_state_t load_state;
} img_entry_t;

typedef struct
{
  i32 total_loader_count;

  img_entry_t* img_entries;

  i32 filtered_img_count;
  i32* filtered_img_idxs;

  volatile i64 total_bytes_used;
  i64 total_bytes_limit;  // Not a strict limit.

  volatile i32 viewing_filtered_img_idx;
  volatile i32 first_visible_thumbnail_idx;
  volatile i32 last_visible_thumbnail_idx;

  volatile i64 next_loaded_img_id;
  volatile i64 next_finalized_img_id;
  loaded_img_t loaded_imgs[1024];
} shared_loader_data_t;

typedef struct
{
  i32 thread_idx;
  sem_t* semaphore;
  shared_loader_data_t* shared;
} loader_data_t;

enum
{
  SORT_MODE_FILEPATH,
  SORT_MODE_TIMESTAMP,
  SORT_MODE_FILESIZE,
  SORT_MODE_RANDOM,
  SORT_MODE_PIXELCOUNT,
  SORT_MODE_PROMPT,
  SORT_MODE_MODEL,
  SORT_MODE_SCORE,

  SORT_MODE_COUNT,
};
typedef u32 sort_mode_t;
internal str_t sort_mode_labels[] = {
  str("[f]ilepath"),
  str("[t]imestamp"),
  str("file[s]ize"),
  str("rand[o]m"),
  str("pi[x]elcount"),
  str("[p]rompt"),
  str("[m]odel"),
  str("sco[r]e"),
};

enum
{
  GROUP_MODE_NONE = 0,
  GROUP_MODE_DAY,
  GROUP_MODE_PROMPT,
  GROUP_MODE_MODEL,

  GROUP_MODE_COUNT,
};
typedef u32 group_mode_t;
internal str_t group_mode_labels[] = {
  str("n[o]ne"),
  str("[d]ay"),
  str("[p]prompt"),
  str("[m]odel"),
};

typedef struct search_history_entry_t
{
  struct search_history_entry_t* prev;
  struct search_history_entry_t* next;
  str_t str;
} search_history_entry_t;

typedef struct
{
  i32 win_w;
  i32 win_h;

  b32 vsync;
  b32 linear_sampling;
  b32 zoom_from_original_size;  // If false, fit to window instead.
  b32 alpha_blend;
  b32 debug_font_atlas;

  b32 show_help;
  i32 help_tab_idx;

  b32 show_thumbnails;
  r32 thumbnail_panel_width_ratio;
  r32 thumbnail_scroll_rows;
  i32 thumbnail_columns;
  b32 scroll_thumbnail_into_view;

  i32 show_info;
  r32 info_panel_width_ratio;

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
  i32* custom_glyphs;
  i32 custom_glyphs_first_char_idx;
  i32 custom_glyph_count;
  i32 next_custom_glyph_idx;

  char** input_paths;
  i32 input_path_count;

  img_entry_t* img_entries;
  i32 total_img_capacity;
  i32 total_img_count;

  b32 sorting_modal;
  sort_mode_t sort_mode;
  b32 sort_descending;
  i32* sorted_img_idxs;
  i32 sorted_img_count;
  i32 filtered_idx_viewed_before_sort;
  sort_mode_t prev_sort_mode;
  b32 prev_sort_descending;
  i32* prev_sorted_img_idxs;

  b32 grouping_modal;
  b32 need_to_layout;
  group_mode_t group_mode;
  group_mode_t prev_group_mode;
  r32 last_layout_fs;
  r32 last_layout_thumbnail_h;
  r32 last_layout_filtered_img_count;
  r32 last_layout_group_mode;

  i32* filtered_img_idxs;
  i32* prev_filtered_img_idxs;
  i32 filtered_img_count;
  i32 prev_filtered_img_count;

  i32 viewing_filtered_img_idx;
  i32 target_thumbnail_column;

  u8 clipboard_str_buffer[64 * 1024];
  str_t clipboard_str;

  b32 filtering_modal;
  u8 search_str_buffer[64 * 1024];
  str_t search_str;
  b32 search_changed;  // This only counts edits.
  b32 search_tweaked;  // This also counts moving the cursor.
  i32 sorted_idx_viewed_before_search;
  i64 selection_start;
  i64 selection_end;
  i32 metadata_loaded_count;
  b32 all_metadata_loaded;

  FILE* search_history_file;
  u8 search_history_buffer[4 * 1024 * 1024];  // Make sure this is pointer-size-aligned.
  search_history_entry_t* first_search_history_entry;
  search_history_entry_t* last_search_history_entry;
  search_history_entry_t* selected_search_history_entry;

  shared_loader_data_t shared;
  i32 loader_count;
#define MAX_THREAD_COUNT 16
  pthread_t loader_threads[MAX_THREAD_COUNT];
  loader_data_t loader_data[MAX_THREAD_COUNT];
  sem_t loader_semaphores[MAX_THREAD_COUNT];
  pthread_t metadata_loader_thread;
  sem_t metadata_loader_semaphore;

  int inotify_fd;

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

      if(scroll_class->number == 2 && scroll_class->increment >= 1e-6f)
      {
        state->xi_scroll_x_increment = scroll_class->increment;
      }
      if(scroll_class->number == 3 && scroll_class->increment >= 1e-6f)
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
    || c == '\\'
    || c == '|'
    ;
}

internal b32 is_linewrap_word_separator(u8 c)
{
  return 0
    || c == '-'
    || c == ':'
    || c == '/'
    || c == '\\'
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

// Also replaces newlines by spaces.
internal b32 str_replace_selection(i64 str_capacity, str_t* str,
    i64* selection_start, i64* selection_end, str_t new_contents)
{
  b32 result = false;

  i64 selection_min = min(*selection_start, *selection_end);
  i64 selection_max = max(*selection_start, *selection_end);
  i64 selection_length = selection_max - selection_min;
  i64 str_length_change = new_contents.size - selection_length;

  if(str_length_change < 0 || str->size + str_length_change <= str_capacity)
  {
    if(str_length_change > 0)
    {
      // Move bytes after the insertion to the right.
      for(i64 move_idx = str->size + str_length_change - 1;
          move_idx >= selection_max + str_length_change;
          --move_idx)
      {
        str->data[move_idx] = str->data[move_idx - str_length_change];
      }
    }

    // Insert new bytes.
    for(i64 new_idx = 0;
        new_idx < new_contents.size;
        ++new_idx)
    {
      u8 c = new_contents.data[new_idx];
      if(c == '\n' || c == '\r') { c = ' '; }
      str->data[selection_min + new_idx] = c;
    }

    if(str_length_change < 0)
    {
      // Move bytes after the insertion to the left.
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

internal r64 parse_next_r64(u8** p, u8* end)
{
  r64 result = 0;
  b32 negative = false;

  if(*p < end && **p == '-')
  {
    negative = true;
    ++*p;
  }

  while(*p < end && **p >= '0' && **p <= '9')
  {
    result *= 10;
    result += **p - '0';
    ++*p;
  }

  if(*p < end && **p == '.')
  {
    ++*p;

    i64 divisor = 1;

    while(*p < end && **p >= '0' && **p <= '9')
    {
      result *= 10;
      result += **p - '0';
      divisor *= 10;
      ++*p;
    }

    result /= (r64)divisor;
  }

  if(negative)
  {
    result = -result;
  }

  return result;
}

internal r32 parse_r32(str_t str)
{
  u8* start = str.data;
  u8* end = str.data + str.size;

  return (r32)parse_next_r64(&start, end);
}

internal void* loader_fun(void* raw_data)
{
  loader_data_t* data = (loader_data_t*)raw_data;
  i32 thread_idx = data->thread_idx;
  shared_loader_data_t* shared = data->shared;

  i64 loaded_count_limit = array_count(shared->loaded_imgs) - shared->total_loader_count;
  // i64 thread_bytes_limit = shared->total_bytes_limit / shared->total_loader_count;
  i64 thread_bytes_limit = shared->total_bytes_limit;

  for(;;)
  {
    i32 viewing_filtered_img_idx = shared->viewing_filtered_img_idx;
    i32 range_start_idx = shared->first_visible_thumbnail_idx;
    i32 range_end_idx = shared->last_visible_thumbnail_idx;
    i32 filtered_img_count = shared->filtered_img_count;

    // printf("Loader %d focus %d range_start %d range_end %d\n", thread_idx, viewing_filtered_img_idx, range_start_idx, range_end_idx);

    i32 max_loading_idx = min(filtered_img_count + 1, (range_end_idx - range_start_idx) + 100);
    // i32 max_loading_idx = min(filtered_img_count + 1, 200);
    i64 thread_bytes_used = 0;
    for(i32 loading_idx = 0;
        1
        && loading_idx < max_loading_idx
        && shared->next_loaded_img_id - shared->next_finalized_img_id < loaded_count_limit
        ;
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

      img_entry_t* img_entry = &shared->img_entries[img_idx];

      // printf("- total %ldk, thread %ldk, img %ldk\n", shared->total_bytes_used / 1024, thread_bytes_used / 1024, img_entry->bytes_used / 1024);
      if(shared->total_bytes_used + img_entry->bytes_used > (3 * shared->total_bytes_limit) / 2
          || thread_bytes_used + img_entry->bytes_used > thread_bytes_limit)
      {
        break;
      }

      u32 load_generation = img_entry->load_generation;
      if(__sync_bool_compare_and_swap(&img_entry->load_state, LOAD_STATE_UNLOADED, LOAD_STATE_LOADING))
      {
        i64 loaded_img_id = __sync_fetch_and_add(&shared->next_loaded_img_id, 1);
        loaded_img_t* loaded_img = &shared->loaded_imgs[loaded_img_id % array_count(shared->loaded_imgs)];

        if(loaded_img->load_state != LOAD_STATE_UNLOADED)
        {
          printf("WARNING: Loaded image slot %ld was not unloaded, but will be overwritten!\n", loaded_img_id);
        }

        loaded_img->load_generation = load_generation;
        loaded_img->entry_idx = img_idx;

        loaded_img->bytes_used = 0;
        i32 original_channel_count = 0;
        loaded_img->pixels = stbi_load((char*)img_entry->path.data,  // XXX: This path string may have been freed!
            &loaded_img->w, &loaded_img->h, &original_channel_count, 4);

        if(!loaded_img->pixels)
        {
          loaded_img->w = 0;
          loaded_img->h = 0;
        }
        else
        {
          loaded_img->bytes_used = 4 * loaded_img->w * loaded_img->h;
          __sync_fetch_and_add(&shared->total_bytes_used, loaded_img->bytes_used);
          thread_bytes_used += loaded_img->bytes_used;

#if 1
          if(original_channel_count == 4)
          {
            // Premultiply alpha.
            // printf("Premultiplying alpha.\n");
            for(u64 i = 0; i < (u64)loaded_img->w * (u64)loaded_img->h; ++i)
            {
              if(loaded_img->pixels[4*i + 3] != 255)
              {
                r32 r = loaded_img->pixels[4*i + 0] / 255.0f;
                r32 g = loaded_img->pixels[4*i + 1] / 255.0f;
                r32 b = loaded_img->pixels[4*i + 2] / 255.0f;
                r32 a = loaded_img->pixels[4*i + 3] / 255.0f;

                loaded_img->pixels[4*i + 0] = (u8)(255.0f * a * r + 0.5f);
                loaded_img->pixels[4*i + 1] = (u8)(255.0f * a * g + 0.5f);
                loaded_img->pixels[4*i + 2] = (u8)(255.0f * a * b + 0.5f);
              }
            }
          }
#endif
        }

        __sync_synchronize();
        loaded_img->load_state = LOAD_STATE_LOADED_INTO_RAM;
      }
      else
      {
        thread_bytes_used += img_entry->bytes_used;
      }
    }

    // printf("Loader %d: Waiting on semaphore.\n", thread_idx);
    sem_wait(data->semaphore);
    // printf("Loader %d: Got signal!\n", thread_idx);
  }

  return 0;
}

internal void* metadata_loader_fun(void* raw_data)
{
  state_t* state = (state_t*)raw_data;

  for(;;)
  {
    // printf("Metadata loader: Waiting on semaphore.\n");
    sem_wait(&state->metadata_loader_semaphore);
    // printf("Metadata loader: Got signal!\n");

    state->metadata_loaded_count = 0;

    // TODO: Prioritize loading currently viewed images,
    //       or let the decoder threads parse the metadata too.
    for(i32 img_idx = 0;
        img_idx < state->total_img_count;
        ++img_idx)
    {
      img_entry_t* img = &state->img_entries[img_idx];
      u32 load_generation = img->load_generation;
      // printf("meta %d / %d\n", img_idx, state->total_img_count);

      if(!(img->flags & IMG_FLAG_UNUSED) && load_generation != img->metadata_generation)
      {
        if(img->file_header_data.size)
        {
          zero_struct(img->parameter_strings);
          free(img->file_header_data.data);
          zero_struct(img->file_header_data);
        }

        int fd = open((char*)img->path.data, O_RDONLY);
        if(fd != -1)
        {
          size_t bytes_to_read = 4 * 1024;
          img->file_header_data.data = malloc_array(bytes_to_read, u8);
          ssize_t bytes_actually_read = read(fd, img->file_header_data.data, bytes_to_read);
          // off_t lseek_result = lseek(fd, 0, SEEK_END);
          // if(lseek_result != -1) { img->filesize = lseek_result; }
          close(fd);
          if(bytes_actually_read != -1)
          {
            img->file_header_data.size = bytes_actually_read;

            // Look for PNG metadata.
            if(img->file_header_data.size >= 16)
            {
              u8* ptr = img->file_header_data.data;
              u8* file_end = img->file_header_data.data + img->file_header_data.size;
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
                chunk_size |= *ptr++; chunk_size <<= 8;
                chunk_size |= *ptr++; chunk_size <<= 8;
                chunk_size |= *ptr++; chunk_size <<= 8;
                chunk_size |= *ptr++;

                u8* value_end = ptr + 4 + chunk_size;
                if(value_end > file_end)
                {
                  bad = true;
                }
                else
                {
                  b32 IHDR_header = (ptr[0] == 'I' && ptr[1] == 'H' && ptr[2] == 'D' && ptr[3] == 'R');
                  b32 tEXt_header = (ptr[0] == 't' && ptr[1] == 'E' && ptr[2] == 'X' && ptr[3] == 't');
                  b32 iTXt_header = (ptr[0] == 'i' && ptr[1] == 'T' && ptr[2] == 'X' && ptr[3] == 't');

                  // https://www.w3.org/TR/2003/REC-PNG-20031110/#11IHDR
                  if(IHDR_header)
                  {
                    if(chunk_size >= 8)
                    {
                      u8* p = ptr + 4;

                      u32 w = 0;
                      w |= *p++; w <<= 8;
                      w |= *p++; w <<= 8;
                      w |= *p++; w <<= 8;
                      w |= *p++;

                      u32 h = 0;
                      h |= *p++; h <<= 8;
                      h |= *p++; h <<= 8;
                      h |= *p++; h <<= 8;
                      h |= *p++;

                      i64 bytes_used = 4 * w * h;

                      // Use atomic compare-exchange to make sure that the other
                      // loader threads have precedence on setting these fields.
                      __sync_bool_compare_and_swap(&img->w, 0, w);
                      __sync_bool_compare_and_swap(&img->h, 0, h);
                      __sync_bool_compare_and_swap(&img->bytes_used, 0, bytes_used);

                      // printf("%.*s: %d x %d\n", PF_STR(img->path), w, h);
                    }
                  }
                  else if(tEXt_header || iTXt_header) // https://www.w3.org/TR/2003/REC-PNG-20031110/#11tEXt
                  {
                    u8* key_start = ptr + 4;

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

                          img->parameter_strings[IMG_STR_SEED] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "\"steps\""))
                        {
                          while(p < value_end && !is_digit(*p)) { ++p; }
                          str_t v = {p};
                          while(p < value_end && is_digit(*p)) { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_SAMPLING_STEPS] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "\"cfg\""))
                        {
                          while(p < value_end && !(is_digit(*p) || *p == '.')) { ++p; }
                          str_t v = {p};
                          while(p < value_end && (is_digit(*p) || *p == '.')) { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_CFG] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "\"sampler_name\""))
                        {
                          str_t v = parse_next_json_str_destructively(&p, value_end);
                          img->parameter_strings[IMG_STR_SAMPLER] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "\"ckpt_name\""))
                        {
                          str_t v = parse_next_json_str_destructively(&p, value_end);
                          v = str_remove_suffix(v, str(".ckpt"));
                          v = str_remove_suffix(v, str(".safetensors"));
                          img->parameter_strings[IMG_STR_MODEL] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "\"batch_size\""))
                        {
                          while(p < value_end && !is_digit(*p)) { ++p; }
                          str_t v = {p};
                          while(p < value_end && is_digit(*p)) { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_BATCH_SIZE] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "\"text\""))
                        {
                          str_t v = parse_next_json_str_destructively(&p, value_end);
                          if(!img->parameter_strings[IMG_STR_POSITIVE_PROMPT].data) { img->parameter_strings[IMG_STR_POSITIVE_PROMPT] = v; }
                          else if(!img->parameter_strings[IMG_STR_NEGATIVE_PROMPT].data) { img->parameter_strings[IMG_STR_NEGATIVE_PROMPT] = v; }
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
                          img->parameter_strings[IMG_STR_NEGATIVE_PROMPT].data = p;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "\nSteps: "))
                        {
                          steps_label_start = p_prev;
                          img->parameter_strings[IMG_STR_SAMPLING_STEPS].data = p;
                        }

                        ++p;
                      }

                      if(negative_prompt_label_start)
                      {
                        img->parameter_strings[IMG_STR_POSITIVE_PROMPT] = str_from_span(value_start, negative_prompt_label_start);
                      }
                      else
                      {
                        img->parameter_strings[IMG_STR_POSITIVE_PROMPT] = str_from_span(value_start, steps_label_start);
                      }

                      if(img->parameter_strings[IMG_STR_NEGATIVE_PROMPT].data)
                      {
                        img->parameter_strings[IMG_STR_NEGATIVE_PROMPT].size = steps_label_start - img->parameter_strings[IMG_STR_NEGATIVE_PROMPT].data;
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

                          img->parameter_strings[IMG_STR_SAMPLING_STEPS] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "Sampler: "))
                        {
                          str_t v = {p};
                          while(p < value_end && *p != ',') { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_SAMPLER] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "CFG scale: "))
                        {
                          str_t v = {p};
                          while(p < value_end && *p != ',') { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_CFG] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "Seed: "))
                        {
                          str_t v = {p};
                          while(p < value_end && *p != ',') { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_SEED] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "Model: "))
                        {
                          str_t v = {p};
                          while(p < value_end && *p != ',') { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_MODEL] = v;
                        }
                        else if(advance_if_prefix_matches(&p, value_end, "Score: "))
                        {
                          str_t v = {p};
                          while(p < value_end && *p != ',') { ++p; }
                          v.size = p - v.data;

                          img->parameter_strings[IMG_STR_SCORE] = v;
                        }

                        ++p;
                      }
                    }

                    if(!img->parameter_strings[IMG_STR_GENERATION_PARAMETERS].size)
                    {
                      img->parameter_strings[IMG_STR_GENERATION_PARAMETERS] = value;
                    }

                    // Parse r32 values.
                    struct
                    {
                      img_str_t str_idx;
                      parsed_r32_t parsed_idx;
                    } parse_tasks[] = {
                      { IMG_STR_SAMPLING_STEPS, PARSED_R32_SAMPLING_STEPS },
                      { IMG_STR_CFG,            PARSED_R32_CFG },
                      { IMG_STR_SCORE,          PARSED_R32_SCORE },
                    };
                    for_count(task_idx, array_count(parse_tasks))
                    {
                      str_t param_str = img->parameter_strings[parse_tasks[task_idx].str_idx];
                      r32* parsed_ptr = &img->parsed_r32s[parse_tasks[task_idx].parsed_idx];
                      if(param_str.size > 0)
                      {
                        *parsed_ptr = parse_r32(param_str);
                        // printf("[%d]: \"%.*s\" -> %f\n", parse_tasks[task_idx].parsed_idx, PF_STR(param_str), *parsed_ptr);
                      }
                    }
                  }
                }

                ptr += 4 + chunk_size + 4;

                bad |= (ptr + 8 >= file_end);
              }
            }

#if 0
            printf("\n%.*s\n", PF_STR(img->path));
#define P(x) printf("  " #x ": %.*s\n", (int)img->parameter_strings[x].size, img->parameter_strings[x].data);
            // P(IMG_STR_GENERATION_PARAMETERS);
            P(IMG_STR_POSITIVE_PROMPT);
            P(IMG_STR_NEGATIVE_PROMPT);
            P(IMG_STR_SEED);
            P(IMG_STR_BATCH_SIZE);
            P(IMG_STR_MODEL);
            P(IMG_STR_SAMPLER);
            P(IMG_STR_SAMPLING_STEPS);
            P(IMG_STR_CFG);
#undef P
#endif

            img->metadata_generation = load_generation;
          }
        }
      }

      ++state->metadata_loaded_count;
      // usleep(200);
    }
  }

  return 0;
}

internal void unload_texture(state_t* state, img_entry_t* unload)
{
  if(unload->lru_prev)
  {
    unload->lru_prev->lru_next = unload->lru_next;
  }
  else if(state->lru_first == unload)
  {
    state->lru_first = unload->lru_next;
  }

  if(unload->lru_next)
  {
    unload->lru_next->lru_prev = unload->lru_prev;
  }
  else if(state->lru_last == unload)
  {
    state->lru_last = unload->lru_prev;
  }

  unload->lru_prev = 0;
  unload->lru_next = 0;

  // printf("Unloading entry %ld\n", unload - state->img_entries);

  if(unload->texture_id)
  {
    glDeleteTextures(1, &unload->texture_id);
    unload->texture_id = 0;
  }

  if(unload->pixels)
  {
    stbi_image_free(unload->pixels);
    unload->pixels = 0;
    __sync_fetch_and_sub(&state->shared.total_bytes_used, unload->bytes_used);
  }

  // unload->w = 0;
  // unload->h = 0;

  // unload->bytes_used = 0;
  // TODO: Make sure to update the remembered bytes_used on refresh,
  //       if it gets used for loading prediction.

  __sync_synchronize();
  unload->load_state = LOAD_STATE_UNLOADED;
}

internal b32 upload_img_texture(state_t* state, img_entry_t* img)
{
  u64 nsecs_start = get_nanoseconds();
  // i32 num_deletions = 0;
  i32 num_uploads = 0;

  b32 result = false;

  if(img->load_state == LOAD_STATE_LOADED_INTO_RAM)
  {
    // if(!(img->flags & IMG_FLAG_FAILED_TO_LOAD))
    if(!img->texture_id && img->pixels)
    {
      glGenTextures(1, &img->texture_id);

      glBindTexture(GL_TEXTURE_2D, img->texture_id);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
      glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
      if(state->linear_sampling)
      {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
      }
      else
      {
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
      }
      glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->pixels);
      ++num_uploads;

      // GLint tmp = 0;
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
  }
  else
  {
    result = true;
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
  return max(5, (i32)(0.01f * state->win_w + 0.5f));
}

internal i32 get_effective_thumbnail_panel_width(state_t* state)
{
  i32 result = 0;
  if(state->show_thumbnails)
  {
    result = max(2, min(state->win_w - 10, state->win_w * state->thumbnail_panel_width_ratio));
  }
  return result;
}

internal r32 get_thumbnail_size(state_t* state)
{
  return max(1.0f,
      (r32)(get_effective_thumbnail_panel_width(state) - get_scrollbar_width(state) - 2)
      / (r32)state->thumbnail_columns);
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

internal r32 get_thumbnail_rows(state_t* state)
{
  r32 result = 1;
  r32 thumbnail_h = get_thumbnail_size(state);
  if(thumbnail_h > 0 && state->filtered_img_count > 0)
  {
    img_entry_t* last_image = get_filtered_img(state, state->filtered_img_count - 1);
    result = -last_image->thumbnail_y / thumbnail_h + 1;
  }
  return result;
}

internal void clamp_thumbnail_scroll_rows(state_t* state)
{
  r32 min_row = 0;
  r32 max_row = 0;
  r32 thumbnail_h = get_thumbnail_size(state);
  if(thumbnail_h > 0 && state->filtered_img_count > 0)
  {
    img_entry_t* last_image = get_filtered_img(state, state->filtered_img_count - 1);
    r32 thumbnail_rows = get_thumbnail_rows(state);
    max_row = thumbnail_rows - state->win_h / thumbnail_h + 1;
  }
  state->thumbnail_scroll_rows = max(min_row, min(max_row, state->thumbnail_scroll_rows));
}

internal void clamp_thumbnail_columns(state_t* state)
{
  state->thumbnail_columns = clamp(1, 32, state->thumbnail_columns);
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

#define COMPARE_SCALARS(a, b) ((a) < (b) ? -1 : ((a) > (b) ? 1 : 0))

internal int compare_img_entries(const void* void_a, const void* void_b, void* void_data)
{
  i32 idx_a = *(i32*)void_a;
  i32 idx_b = *(i32*)void_b;
  state_t* state = (state_t*)void_data;
  img_entry_t* img_a = &state->img_entries[idx_a];
  img_entry_t* img_b = &state->img_entries[idx_b];
  int result = 0;

  switch(state->sort_mode)
  {
    case SORT_MODE_TIMESTAMP:
    {
      result = COMPARE_SCALARS(img_a->modified_at_time.tv_sec, img_b->modified_at_time.tv_sec);
      if(result == 0)
      {
        result = COMPARE_SCALARS(img_a->modified_at_time.tv_nsec, img_b->modified_at_time.tv_nsec);
      }
    } break;

    case SORT_MODE_FILESIZE:
    {
      result = COMPARE_SCALARS(img_a->filesize, img_b->filesize);
    } break;

    case SORT_MODE_RANDOM:
    {
      result = COMPARE_SCALARS(img_a->random_number, img_b->random_number);
    } break;

    case SORT_MODE_PIXELCOUNT:
    {
      i32 pixels_a = img_a->w * img_a->h;
      i32 pixels_b = img_b->w * img_b->h;
      result = COMPARE_SCALARS(pixels_a, pixels_b);
    } break;

    case SORT_MODE_PROMPT:
    {
      result = str_compare(img_a->parameter_strings[IMG_STR_POSITIVE_PROMPT], img_b->parameter_strings[IMG_STR_POSITIVE_PROMPT]);
      if(!result)
      {
        result = str_compare(img_a->parameter_strings[IMG_STR_NEGATIVE_PROMPT], img_b->parameter_strings[IMG_STR_NEGATIVE_PROMPT]);
      }
    } break;

    case SORT_MODE_MODEL:
    {
      result = str_compare(img_a->parameter_strings[IMG_STR_MODEL], img_b->parameter_strings[IMG_STR_MODEL]);
    } break;

    case SORT_MODE_SCORE:
    {
      result = COMPARE_SCALARS(img_a->parsed_r32s[PARSED_R32_SCORE], img_b->parsed_r32s[PARSED_R32_SCORE]);
    } break;
  }

  if(result == 0)
  {
    // Tie-break.
    result = str_compare(img_a->path, img_b->path);
  }

  if(state->sort_descending)
  {
    result = -result;
  }

  return result;
}

internal void reset_filtered_images(state_t* state)
{
  for_count(i, state->sorted_img_count)
  {
    state->filtered_img_idxs[i] = state->sorted_img_idxs[i];
  }
  state->filtered_img_count = state->sorted_img_count;
}

internal i32 find_sorted_idx_of_img_idx(state_t* state, i32 img_idx)
{
  i32 result = 0;

  for_count(i, state->sorted_img_count)
  {
    if(state->sorted_img_idxs[i] == img_idx)
    {
      result = i;
      break;
    }
  }

  return result;
}

internal i32 find_filtered_idx_of_img_idx(state_t* state, i32 img_idx)
{
  i32 result = 0;

  for_count(i, state->filtered_img_count)
  {
    if(state->filtered_img_idxs[i] == img_idx)
    {
      result = i;
      break;
    }
  }

  return result;
}

internal u32 hash_str(str_t str)
{
  u32 result = 0;
  for_count(i, str.size)
  {
    result *= 1021;
    result += str.data[i];
  }
  return result;
}

internal void refresh_input_paths(state_t* state)
{
  // u64 nsecs_start = get_nanoseconds();

  b32 first_run = (state->sorted_img_count == 0);
  b32 all_files_were_filtered = (state->filtered_img_count == state->sorted_img_count);
  i32 prev_viewing_img_idx = state->filtered_img_idxs[state->viewing_filtered_img_idx];
  state->sorted_img_count = 0;

  for_count(i, state->filtered_img_count)
  {
    img_entry_t* img = &state->img_entries[state->filtered_img_idxs[i]];
    img->flags |= IMG_FLAG_FILTERED;
  }

  // Build path -> img_idx hash map, internally linked.
  u32 path_hash_size = 64 * 1024;
  while(path_hash_size < 4 * state->total_img_count) { path_hash_size *= 2; }
  i32* path_hashes = malloc_array(path_hash_size, i32);
  for_count(i, path_hash_size) { path_hashes[i] = -1; }
  for_count(img_idx, state->total_img_count)
  {
    img_entry_t* img = &state->img_entries[img_idx];
    if(!(img->flags & IMG_FLAG_UNUSED))
    {
      u32 hash = hash_str(img->path);
      i32 slot = (i32)(hash % path_hash_size);
      for(i32 offset = 0;
          offset < path_hash_size;
          ++offset)
      {
        i32* entry = &path_hashes[(slot + offset) % path_hash_size];
        if(*entry == -1)
        {
          *entry = img_idx;
          break;
        }
      }

      state->img_entries[img_idx].flags |= IMG_FLAG_UNUSED;
    }
  }

  char** paths = malloc_array(state->total_img_capacity, char*);
  i32 path_count = 0;

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
      while((dirent = readdir(dir)) && path_count < state->total_img_capacity)
      {
        is_a_dir = true;
        char* path = dirent->d_name;

        if(path[0] != '.' && dirent->d_type != DT_DIR)
        {
          char* full_path = 0;
          if(asprintf(&full_path, "%s/%s", arg, path) != -1)
          {
            paths[path_count++] = full_path;
          }
        }
      }
      closedir(dir);
    }

    if(!is_a_dir && path_count < state->total_img_capacity)
    {
      paths[path_count++] = strdup(arg);
    }

    if(state->inotify_fd != -1)
    {
      int watch_fd = inotify_add_watch(state->inotify_fd, arg,
          IN_CLOSE_WRITE | IN_CREATE | IN_DELETE | IN_DELETE_SELF /* | IN_MODIFY */ | IN_MOVE_SELF | IN_MOVED_FROM | IN_MOVED_TO
          | IN_EXCL_UNLINK /* | IN_ONLYDIR */);
    }
  }

  i32 first_possible_unused_img_idx = 0;

  for(i32 pass = 0;
      pass <= 1;
      ++pass)
  {
    for(i32 path_idx = 0;
        path_idx < path_count;
        )
    {
      str_t new_path = wrap_str(paths[path_idx]);

      i32 img_idx = -1;

      // Try to match existing image entries by path.
      if(pass == 0)
      {
        u32 hash = hash_str(new_path);
        i32 slot = (i32)(hash % path_hash_size);
        for(i32 offset = 0;
            offset < path_hash_size;
            ++offset)
        {
          i32 entry = path_hashes[(slot + offset) % path_hash_size];
          if(entry == -1) { break; }
          if(str_eq(new_path, state->img_entries[entry].path))
          {
            img_idx = entry;
            break;
          }
        }
      }

      if(pass == 1)
      {
        // Try to fill up unused slots (in case files got deleted since last time).
        for(i32 i = first_possible_unused_img_idx;
            i < state->total_img_count;
            ++i)
        {
          if(state->img_entries[i].flags & IMG_FLAG_UNUSED)
          {
            img_idx = i;
            break;
          }
        }

        if(img_idx == -1 && state->total_img_count < state->total_img_capacity)
        {
          img_idx = state->total_img_count++;
        }

        first_possible_unused_img_idx = img_idx + 1;
      }

      if(img_idx != -1)
      {
        img_entry_t* img = &state->img_entries[img_idx];
        img->flags &= ~IMG_FLAG_UNUSED;
        str_t old_path = img->path;
        b32 path_changed = !str_eq(old_path, new_path);

        if(path_changed)
        {
          img->path = new_path;
          // TODO: Right now, this will leak the memory of the old paths.
          //       Those must be freed up, without risking the loader threads
          //       trying to read from that freed memory.
          //       Also, changing the path while the loaders are running might be problematic.
        }
        else
        {
          if(new_path.data)
          {
            free(new_path.data);
            zero_struct(new_path);
          }
        }

        b32 file_may_have_changed = true;
        struct stat stats = {0};
        if(stat((char*)img->path.data, &stats) == 0)
        {
          if(stats.st_mtim.tv_sec == img->modified_at_time.tv_sec &&
              stats.st_mtim.tv_nsec == img->modified_at_time.tv_nsec &&
              stats.st_size == img->filesize)
          {
            file_may_have_changed = false;
          }
          img->modified_at_time = stats.st_mtim;
          img->filesize = stats.st_size;
        }

        if(path_changed || file_may_have_changed)
        {
          unload_texture(state, img);
          img->bytes_used = 0;

          // If this image is still being loaded, it should be re-triggered by the
          // code handling the loaded image, since load_generation will differ.
          ++img->load_generation;
          img->load_state = LOAD_STATE_UNLOADED;
        }

        if(!img->random_number)
        {
          img->random_number = max(1, (u32)rand());
        }

        state->sorted_img_idxs[state->sorted_img_count++] = img_idx;

        // Remove this handled path so the next pass can ignore it.
        paths[path_idx] = paths[path_count - 1];
        --path_count;
      }
      else
      {
        ++path_idx;
      }
    }
  }

  if(path_count > 0)
  {
    fprintf(stderr, "Warning: %d paths left unhandled.\n", path_count);
  }

  free(paths);
  free(path_hashes);

  qsort_r(state->sorted_img_idxs, state->sorted_img_count, sizeof(state->sorted_img_idxs[0]), compare_img_entries, state);

  state->filtered_img_count = 0;
  for_count(i, state->sorted_img_count)
  {
    i32 img_idx = state->sorted_img_idxs[i];
    if(all_files_were_filtered || (state->img_entries[img_idx].flags & IMG_FLAG_FILTERED))
    {
      state->filtered_img_idxs[state->filtered_img_count] = img_idx;
      if(img_idx == prev_viewing_img_idx) { state->viewing_filtered_img_idx = state->filtered_img_count; }
      ++state->filtered_img_count;
    }
  }
  if(first_run)
  {
    state->viewing_filtered_img_idx = 0;
  }
  else
  {
    state->viewing_filtered_img_idx = max(0, min(state->viewing_filtered_img_idx, state->filtered_img_count - 1));
  }

  for_count(i, state->total_img_count)
  {
    img_entry_t* img = &state->img_entries[i];
    if(img->flags & IMG_FLAG_UNUSED) { img->flags &= ~IMG_FLAG_MARKED; }
    img->flags &= ~IMG_FLAG_FILTERED;
  }

  sem_post(&state->metadata_loader_semaphore);
  state->all_metadata_loaded = false;

  // u64 nsecs_end = get_nanoseconds();
  // printf("refresh: %.6f s\n", 1e-9 * (r64)(nsecs_end - nsecs_start));
}

enum
{
  DRAW_STR_MEASURE_ONLY = (1 << 0),
};
typedef u32 draw_str_flags_t;

internal r32 draw_str_advanced(state_t* state, draw_str_flags_t flags,
    r32 x_scale_factor, r32 y_scale, r32 start_x, r32 y, str_t str, i32* last_glyph_ptr)
{
  r32 x = start_x;
  b32 measure_only = (flags & DRAW_STR_MEASURE_ONLY);
  u32 last_glyph = last_glyph_ptr ? *last_glyph_ptr : 0;

  if(state->font_texture_id && str.size > 0)
  {
    if(!measure_only)
    {
      glBindTexture(GL_TEXTURE_2D, state->alpha_blend ? state->font_texture_id : 0);
      glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
      glEnable(GL_BLEND);
    }

    r32 x_scale = x_scale_factor * y_scale;

    u8* str_end = str.data + str.size;

    for(u8* str_ptr = str.data;
        str_ptr < str_end;
       )
    {
      u32 codepoint = decode_utf8(&str_ptr, str_end);
      if(codepoint == '\t') { codepoint = ' '; }

      i32 glyph = stbtt_FindGlyphIndex(&state->font, codepoint);

      i32 x_advance, left_side_bearing;
      stbtt_GetGlyphHMetrics(&state->font, glyph, &x_advance, &left_side_bearing);

      if(last_glyph)
      {
        x += x_scale * stbtt_GetGlyphKernAdvance(&state->font, last_glyph, glyph)
          * state->stb_font_scale / state->font_char_w;
      }

      if(!measure_only)
      {
        i32 char_idx = (i32)codepoint - (i32)state->fixed_codepoint_range_start;
        if(char_idx < 0 || char_idx >= state->fixed_codepoint_range_length)
        {
          char_idx = 0;
          for(i32 custom_idx = 0;
              custom_idx < state->custom_glyph_count;
              ++custom_idx)
          {
            if(state->custom_glyphs[custom_idx] == glyph)
            {
              char_idx = custom_idx + state->fixed_codepoint_range_length;
              break;
            }
          }

          if(!char_idx)
          {
            char_idx = state->next_custom_glyph_idx + state->fixed_codepoint_range_length;
            state->custom_glyphs[state->next_custom_glyph_idx] = glyph;

            i32 row = char_idx / state->chars_per_font_row;
            i32 col = char_idx % state->chars_per_font_row;

            // printf("uploading codepoint %d\n", codepoint);
            // printf("  char_idx %d, row %d, col %d\n", char_idx, row, col);

            u8* texels_start = state->font_texels + row * state->font_char_h * state->font_texture_w + col * state->font_char_w;
            for_count(j, state->font_char_h)
            {
              for_count(i, state->font_char_w)
              {
                texels_start[j * state->font_texture_w + i] = 0;
              }
            }
            stbtt_MakeGlyphBitmap(&state->font, texels_start,
                state->font_char_w, state->font_char_h, state->font_texture_w,
                state->stb_font_scale, state->stb_font_scale, glyph);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, state->font_texture_w);
            glTexSubImage2D(GL_TEXTURE_2D, 0,
                col * state->font_char_w, row * state->font_char_h, state->font_char_w, state->font_char_h,
                GL_ALPHA, GL_UNSIGNED_BYTE, texels_start);
            glPixelStorei(GL_UNPACK_ROW_LENGTH, 0);

            state->next_custom_glyph_idx = (state->next_custom_glyph_idx + 1) % state->custom_glyph_count;
          }
        }

        i32 ix0, iy0, ix1, iy1;
        stbtt_GetGlyphBitmapBox(&state->font, glyph, state->stb_font_scale, state->stb_font_scale, &ix0, &iy0, &ix1, &iy1);

        // Without this extra padding, some of the antialiased edges get cut off slightly.
        ix1 += 1;
        iy1 += 1;

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
      last_glyph = glyph;
    }
  }

  if(last_glyph_ptr)
  {
    *last_glyph_ptr = last_glyph;
  }

  return x - start_x;
}

internal r32 draw_str(state_t* state, draw_str_flags_t flags, r32 y_scale, r32 start_x, r32 y, str_t str)
{
  return draw_str_advanced(state, flags, 1, y_scale, start_x, y, str, 0);
}

typedef struct
{
  state_t* state;
  r32 fs;
  r32 x0;
  r32 x1;
  str_t remaining_text;
  i32 line_idx;

  r32 line_end_x;
  b32 finished;
} wrapped_text_ctx_t;

internal wrapped_text_ctx_t begin_wrapped_text(state_t* state, r32 fs, r32 x0, r32 x1, str_t text)
{
  wrapped_text_ctx_t result = {0};
  result.state = state;
  result.fs = fs;
  result.x0 = x0;
  result.x1 = x1;
  result.remaining_text = text;

  return result;
}

internal str_t wrap_next_line(wrapped_text_ctx_t* ctx, r32 x)
{
  u8* text_end = ctx->remaining_text.data + ctx->remaining_text.size;
  str_t result = { text_end, 0 };

  if(!ctx->finished)
  {
    b32 first_word_can_get_split = (ctx->line_idx != 0 || x <= ctx->x0);
    i32 last_glyph = 0;
    u8* line_start = ctx->remaining_text.data;
    u8* line_end = line_start;
    u8* remainder_start = line_start;
    u8* chr_end = line_start;

    ctx->line_end_x = x;

    for(;;)
    {
      if(chr_end >= text_end)
      {
        line_end = text_end;
        remainder_start = text_end;
        ctx->line_end_x = x;
        ctx->finished = true;
        break;
      }

      u8* chr_start = chr_end;
      ++chr_end;
      while(chr_end < text_end && is_utf8_continuation_byte(*chr_end))
      {
        ++chr_end;
      }

      if(*chr_start == '\n')
      {
        line_end = chr_start;
        remainder_start = chr_start + 1;
        ctx->line_end_x = x;
        break;
      }
      else if(*chr_start == ' ')
      {
        line_end = chr_start;
        remainder_start = chr_start + 1;
        ctx->line_end_x = x;
      }

      x += draw_str_advanced(ctx->state, DRAW_STR_MEASURE_ONLY, 1, ctx->fs, 0, 0,
          str_from_span(chr_start, chr_end), &last_glyph);

      if(x > ctx->x1)
      {
        if(first_word_can_get_split && remainder_start == line_start)
        {
          line_end = chr_start > line_start ? chr_start : chr_end;
          remainder_start = line_end;
          ctx->line_end_x = x;
        }
        break;
      }

      if(chr_start > line_start && is_linewrap_word_separator(*chr_start))
      {
        line_end = chr_start + 1;
        remainder_start = chr_start + 1;
        ctx->line_end_x = x;
      }
    }

    result = str_from_span(line_start, line_end);
    ctx->remaining_text = str_from_span(remainder_start, text_end);
  }

  return result;
}

internal b32 finish_wrapped_line(wrapped_text_ctx_t* ctx, r32* x, r32* y)
{
  if(!ctx->finished)
  {
    *x = ctx->x0;
    *y -= ctx->fs;
    ++ctx->line_idx;
  }

  return !ctx->finished;
}

internal void draw_wrapped_text(state_t* state,
    r32 fs, r32 x0, r32 x1, r32* x, r32* y, str_t text)
{
  wrapped_text_ctx_t wrap_ctx = begin_wrapped_text(state, fs, x0, x1, text);
  do
  {
    str_t line = wrap_next_line(&wrap_ctx, *x);
    *x += draw_str(state, 0, fs, *x, *y, line);
  } while(finish_wrapped_line(&wrap_ctx, x, y));
}

internal b32 sort_mode_needs_metadata(sort_mode_t mode)
{
  return mode != SORT_MODE_FILEPATH
    && mode != SORT_MODE_TIMESTAMP
    && mode != SORT_MODE_FILESIZE
    && mode != SORT_MODE_RANDOM;
}

internal b32 add_search_history_entry(state_t* state, str_t str)
{
  b32 got_added = false;

  if(!state->last_search_history_entry || !str_eq(str, state->last_search_history_entry->str))
  {
    search_history_entry_t* entry = 0;
    size_t entry_size = sizeof(search_history_entry_t) + str.size;
    u8* buffer_end = state->search_history_buffer + sizeof(state->search_history_buffer);

    if(!state->last_search_history_entry)
    {
      entry = state->first_search_history_entry = state->last_search_history_entry =
        (search_history_entry_t*)state->search_history_buffer;
      entry->prev = entry->next = 0;
    }
    else
    {
      str_t last_str = state->last_search_history_entry->str;
      u8* new_base = (u8*)((u64)(last_str.data + last_str.size + 7) & ~7);
      if(new_base + entry_size > buffer_end)
      {
        new_base = state->search_history_buffer;
        // If we're rolling back to the start of the buffer,
        // delete all the starting entries from the end of the buffer
        // to make sure the start of the entry chain does not cross the end-boundary.
        while(state->first_search_history_entry && state->first_search_history_entry > state->last_search_history_entry)
        {
          state->first_search_history_entry = state->first_search_history_entry->next;
          state->first_search_history_entry->prev = 0;
        }
      }
      while(state->first_search_history_entry
          && new_base <= (u8*)state->first_search_history_entry
          && (u8*)state->first_search_history_entry < new_base + entry_size)
      {
        state->first_search_history_entry = state->first_search_history_entry->next;
      }
      if(state->first_search_history_entry)
      {
        state->first_search_history_entry->prev = 0;
        entry = (search_history_entry_t*)new_base;
        entry->prev = state->last_search_history_entry;
        state->last_search_history_entry->next = entry;
        state->last_search_history_entry = entry;
        entry->next = 0;
      }
      else
      {
        entry = state->first_search_history_entry = state->last_search_history_entry =
          (search_history_entry_t*)state->search_history_buffer;
        entry->prev = entry->next = 0;
      }
    }

    if((u8*)entry + entry_size > buffer_end)
    {
      entry = 0;
    }

    if(entry)
    {
      entry->str.data = (u8*)entry + sizeof(*entry);
      entry->str.size = str.size;
      copy_bytes(str.size, str.data, entry->str.data);
      got_added = true;
    }
  }

  return got_added;
}

internal void start_search(state_t* state)
{
  state->filtering_modal = true;

  state->selection_start = state->search_str.size;
  state->selection_end   = state->search_str.size;
  for_count(i, state->filtered_img_count) { state->prev_filtered_img_idxs[i] = state->filtered_img_idxs[i]; }
  state->prev_filtered_img_count = state->filtered_img_count;
  state->sorted_idx_viewed_before_search = find_sorted_idx_of_img_idx(state,
      state->filtered_img_idxs[state->viewing_filtered_img_idx]);
  state->selected_search_history_entry = state->last_search_history_entry;

  state->search_changed = true;
  state->search_tweaked = false;
}

internal r32 get_font_size(state_t* state)
{
  r32 win_min_side = min(state->win_w, state->win_h);
  r32 fs = clamp(12, 36, (26.0f / 1080.0f) * win_min_side);
  return fs;
}

internal b32 group_eq(state_t* state, img_entry_t* a, img_entry_t* b)
{
  b32 result = true;

  switch(state->group_mode)
  {
    case GROUP_MODE_NONE:
    {
    } break;

    case GROUP_MODE_DAY:
    {
      struct tm ta = {0};
      struct tm tb = {0};
      localtime_r(&a->modified_at_time.tv_sec, &ta);
      localtime_r(&b->modified_at_time.tv_sec, &tb);
      result = true
        && ta.tm_year == tb.tm_year
        && ta.tm_mon  == tb.tm_mon
        && ta.tm_mday == tb.tm_mday
        ;
    } break;

    case GROUP_MODE_PROMPT:
    {
      result =
        str_eq(a->parameter_strings[IMG_STR_POSITIVE_PROMPT], b->parameter_strings[IMG_STR_POSITIVE_PROMPT]) &&
        str_eq(a->parameter_strings[IMG_STR_NEGATIVE_PROMPT], b->parameter_strings[IMG_STR_NEGATIVE_PROMPT]);
    } break;

    case GROUP_MODE_MODEL:
    {
      result = str_eq(a->parameter_strings[IMG_STR_MODEL], b->parameter_strings[IMG_STR_MODEL]);
    } break;
  }

  return result;
}

internal void group_and_layout_thumbnails(state_t* state)
{
  // u64 nsecs_start = get_nanoseconds();

  r32 fs = get_font_size(state);
  r32 thumbnail_h = get_thumbnail_size(state);

  if(state->need_to_layout
      || fs != state->last_layout_fs
      || thumbnail_h != state->last_layout_thumbnail_h
      || state->filtered_img_count != state->last_layout_filtered_img_count
      || state->group_mode != state->last_layout_group_mode
      )
  {
    i32 current_group = -1;
    i32 col = 0;
    r32 y = 0;
    img_entry_t* prev_img = 0;
    for(i32 filtered_idx = 0;
        filtered_idx < state->filtered_img_count;
        ++filtered_idx)
    {
      img_entry_t* img = &state->img_entries[state->filtered_img_idxs[filtered_idx]];

      if(current_group == -1 || !group_eq(state, img, prev_img))
      {
        if(current_group != -1)
        {
          col = 0;
          y -= thumbnail_h;
        }
        if(state->group_mode != GROUP_MODE_NONE)
        {
          y -= 1.5f * fs;
        }
        if(state->group_mode == GROUP_MODE_PROMPT && img->parameter_strings[IMG_STR_NEGATIVE_PROMPT].size > 0)
        {
          y -= fs;
        }
        ++current_group;
      }
      else
      {
        ++col;
        if(col >= state->thumbnail_columns)
        {
          col = 0;
          y -= thumbnail_h;
        }
      }

      img->thumbnail_column = col;
      img->thumbnail_y = y;
      img->thumbnail_group = current_group;
      prev_img = img;
    }

    // u64 nsecs_end = get_nanoseconds();
    // printf("layout: %.3f ms\n", 1e-6 * (r64)(nsecs_end - nsecs_start));
  }

  state->last_layout_fs = fs;
  state->last_layout_thumbnail_h = thumbnail_h;
  state->last_layout_filtered_img_count = state->filtered_img_count;
  state->last_layout_group_mode = state->group_mode;
  state->need_to_layout = false;
}

int main(int argc, char** argv)
{
#if !RELEASE
  debug_out = fopen("/tmp/i2x-debug.log", "wb");
  // if(!debug_out) { debug_out = stderr; }
#endif
  srand(get_nanoseconds());

  state_t* state = malloc_struct(state_t);
  zero_struct(*state);
  state->loader_count = 7;
  state->shared.total_bytes_limit = 1 * 1024 * 1024 * 1024LL;

  char* xdg_state_home = getenv("XDG_STATE_HOME");
  char* default_search_history_path = 0;
  if(xdg_state_home)
  {
    asprintf(&default_search_history_path, "%s/i2x/searches.txt", xdg_state_home);
  }
  else
  {
    char* home = getenv("HOME");
    if(home)
    {
      asprintf(&default_search_history_path, "%s/.local/state/i2x/searches.txt", home);
    }
  }

  if(argc <= 1
      || zstr_eq(argv[1], "--help")
      || zstr_eq(argv[1], "-h"))
  {
    printf("Usage: %s <image files and directories>\n", argv[0]);
    printf("\n");
    printf("Press F1 for GUI help.\n");
    printf("\n");
    printf("Directories get expanded (one level, not recursive).\n");
    printf("If only one file is passed, its containing directory is opened, and the file focused.\n");
    printf("\n");
    printf("The following environment variables are used:\n");
    printf("I2X_SORT_ORDER:      Sets an initial sort order (Default: path). One of:\n");
    printf("                     path, time, filesize, random, pixelcount*, prompt*, model*, score*.\n");
    printf("                     Can be suffixed by \"_desc\" for descending (default is ascending).\n");
    printf("                     *: Orderings which depend on metadata may delay startup.\n");
    printf("I2X_INIT_SEARCH:     Starts up with the given search query active.\n");
    printf("I2X_SEARCH_HISTORY:  When set, persists the search history to disk.\n");
    printf("                     The string this is set to determines the path.\n");
    printf("                     When set to an empty string, defaults to:\n");
    printf("                     %s\n", default_search_history_path);
    printf("I2X_DISABLE_INOTIFY: Disables automatic directory refresh using inotify.\n");
    printf("I2X_DISABLE_XINPUT2: Disables XInput2 handling, which allows\n");
    printf("                     smooth scrolling and raw sub-pixel mouse motion,\n");
    printf("                     but can be glitchy.\n");
    printf("I2X_LOADER_THREADS:  The number of image-loader threads. Default: %d\n", state->loader_count);
    printf("I2X_TARGET_VRAM_MB:  Video memory usage to target in MiB, very roughly.\n");
    printf("                     Might use more than 2x this amount. Default: %ld\n",
        state->shared.total_bytes_limit / (1024 * 1024));
    printf("I2X_TTF_PATH:        Use an external font file instead of the internal one.\n");
    printf("\n");
    printf("Example invocation:\n  I2X_DISABLE_XINPUT2=1 I2X_LOADER_THREADS=3 I2X_SORT_ORDER=time_desc %s\n", argv[0]);
    return 0;
  }

  Display* display = XOpenDisplay(0);
  if(display)
  {
    int screen_number = 0;
    Window root_window = RootWindow(display, screen_number);

    b32 xi_available = false;
    int xi_opcode = 0;
#if 1
    if(!getenv("I2X_DISABLE_XINPUT2"))
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

        // TODO: Handle WM_DELETE_WINDOW if a close-confirmation ever gets required,
        //       or to not get an "X connection to :0 broken" message on some WMs.
        //       https://tronche.com/gui/x/icccm/sec-4.html#s-4.2.8.1

        Window window = XCreateWindow(display, root_window,
            0, 0, WINDOW_INIT_W, WINDOW_INIT_H,
            0, visual_info->depth, InputOutput, glx_visual,
            CWEventMask | CWColormap, &window_attributes);

        XFree(visual_info);
        GLXWindow glx_window = glXCreateWindow(display, glx_config, window, 0);

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
          // XISetMask(mask, XI_RawMotion);  // This can't be used reliably for mouse deltas :(
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

        char* sort_order_envvar = getenv("I2X_SORT_ORDER");
        if(sort_order_envvar)
        {
          str_t str = wrap_str(sort_order_envvar);
          if(0) {}
          else if(str_eq_ignoring_case(str, str("path"))) { state->sort_mode = SORT_MODE_FILEPATH; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("path_desc"))) { state->sort_mode = SORT_MODE_FILEPATH; state->sort_descending = true; }
          else if(str_eq_ignoring_case(str, str("time"))) { state->sort_mode = SORT_MODE_TIMESTAMP; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("time_desc"))) { state->sort_mode = SORT_MODE_TIMESTAMP; state->sort_descending = true; }
          else if(str_eq_ignoring_case(str, str("filesize"))) { state->sort_mode = SORT_MODE_FILESIZE; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("filesize_desc"))) { state->sort_mode = SORT_MODE_FILESIZE; state->sort_descending = true; }
          else if(str_eq_ignoring_case(str, str("random"))) { state->sort_mode = SORT_MODE_RANDOM; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("random_desc"))) { state->sort_mode = SORT_MODE_RANDOM; state->sort_descending = true; }
          else if(str_eq_ignoring_case(str, str("pixelcount"))) { state->sort_mode = SORT_MODE_PIXELCOUNT; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("pixelcount_desc"))) { state->sort_mode = SORT_MODE_PIXELCOUNT; state->sort_descending = true; }
          else if(str_eq_ignoring_case(str, str("prompt"))) { state->sort_mode = SORT_MODE_PROMPT; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("prompt_desc"))) { state->sort_mode = SORT_MODE_PROMPT; state->sort_descending = true; }
          else if(str_eq_ignoring_case(str, str("model"))) { state->sort_mode = SORT_MODE_MODEL; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("model_desc"))) { state->sort_mode = SORT_MODE_MODEL; state->sort_descending = true; }
          else if(str_eq_ignoring_case(str, str("score"))) { state->sort_mode = SORT_MODE_SCORE; state->sort_descending = false; }
          else if(str_eq_ignoring_case(str, str("score_desc"))) { state->sort_mode = SORT_MODE_SCORE; state->sort_descending = true; }
          else
          {
            fprintf(stderr, "Ignoring unknown I2X_SORT_ORDER: \"%.*s\"\n", PF_STR(str));
          }
        }

        if(!getenv("I2X_DISABLE_INOTIFY"))
        {
          state->inotify_fd = inotify_init1(IN_NONBLOCK);
        }
        else
        {
          state->inotify_fd = -1;
        }

        if(state->inotify_fd == -1)
        {
          fprintf(stderr, "No inotify available.\n");
        }

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
        state->img_entries = malloc_array_zero(state->total_img_capacity, img_entry_t);
        state->sorted_img_idxs = malloc_array_zero(state->total_img_capacity, i32);
        state->prev_sorted_img_idxs = malloc_array_zero(state->total_img_capacity, i32);
        state->filtered_img_idxs = malloc_array_zero(state->total_img_capacity, i32);
        state->prev_filtered_img_idxs = malloc_array_zero(state->total_img_capacity, i32);

        sem_init(&state->metadata_loader_semaphore, 0, 0);
        pthread_create(&state->metadata_loader_thread, 0, metadata_loader_fun, state);

        refresh_input_paths(state);

        {
          char* search_history_envvar = getenv("I2X_SEARCH_HISTORY");
#if !ALWAYS_PERSIST_SEARCH_HISTORY
          if(search_history_envvar)
#endif
          {
            char* search_history_path = default_search_history_path;
            if(search_history_envvar && search_history_envvar[0] != 0)
            {
              search_history_path = search_history_envvar;
            }

            // Create parent directories first.
            for(i32 char_idx = 0;
                search_history_path[char_idx] != 0;
                ++char_idx)
            {
              if(search_history_path[char_idx] == '/')
              {
                search_history_path[char_idx] = 0;
                mkdir(search_history_path, 0700);
                search_history_path[char_idx] = '/';
              }
            }

            state->search_history_file = fopen(search_history_path, "a+b");

            if(state->search_history_file)
            {
              int file_size = 0;
              if(fseek(state->search_history_file, 0, SEEK_END) != -1 &&
                  (file_size = ftell(state->search_history_file)) != -1)
              {
                int read_start = max(0, file_size - (int)(sizeof(state->search_history_buffer) / 2));
                int read_length = file_size - read_start;
                fseek(state->search_history_file, read_start, SEEK_SET);
                u8* contents = (u8*)malloc(read_length);
                if(contents)
                {
                  if(fread(contents, 1, read_length, state->search_history_file) == read_length)
                  {
                    u8* p = contents;
                    u8* contents_end = contents + read_length;

                    if(read_start != 0)
                    {
                      while(p < contents_end && *p != '\n' && *p != '\r') { ++p; }
                    }

                    while(p < contents_end)
                    {
                      str_t str = { p };
                      while(p < contents_end && *p != '\n' && *p != '\r') { ++p; }
                      str.size = p - str.data;
                      while(p < contents_end && (*p == '\n' || *p == '\r')) { ++p; }

                      if(str.size > 0)
                      {
                        add_search_history_entry(state, str);
                      }
                    }
                  }

                  free(contents);
                }
              }
            }
            else
            {
              fprintf(stderr, "Search history file \"%s\" could not be opened.\n", search_history_path);
            }
          }
        }
        add_search_history_entry(state, str(""));

        if(sort_mode_needs_metadata(state->sort_mode))
        {
          while(state->metadata_loaded_count < state->total_img_count)
          {
            usleep(100000);
          }
          state->all_metadata_loaded = true;

          qsort_r(state->sorted_img_idxs, state->sorted_img_count, sizeof(state->sorted_img_idxs[0]), compare_img_entries, state);
          reset_filtered_images(state);
        }

        if(open_single_directory_on.size)
        {
          for_count(i, state->sorted_img_count)
          {
            i32 img_idx = state->sorted_img_idxs[i];
            if(str_has_suffix(state->img_entries[img_idx].path, open_single_directory_on))
            {
              state->viewing_filtered_img_idx = i;
              break;
            }
          }
        }

        state->search_str.data = state->search_str_buffer;
        {
          char* init_search_envvar = getenv("I2X_INIT_SEARCH");
          if(init_search_envvar)
          {
            state->search_str.size = zstr_length(init_search_envvar);
            copy_bytes(state->search_str.size, init_search_envvar, state->search_str.data);
            start_search(state);
          }
        }

        state->font_texture_w = 512;
        state->font_texture_h = 512;
        state->font_char_w = 32;
        state->font_char_h = 32;
        state->chars_per_font_row = state->font_texture_w / state->font_char_w;
        state->chars_per_font_col = state->font_texture_h / state->font_char_h;
        state->fixed_codepoint_range_start = 32;  // ' '
        state->fixed_codepoint_range_length = 127 - state->fixed_codepoint_range_start;  // '~'
        state->custom_glyph_count = state->chars_per_font_row * state->chars_per_font_col - state->fixed_codepoint_range_length;
        state->custom_glyphs = malloc_array(state->custom_glyph_count, i32);

        {
          u8* ttf_data = 0;
          char* ttf_path = getenv("I2X_TTF_PATH");
          if(ttf_path)
          {
            ttf_data = read_file(ttf_path).data;
            if(!ttf_data)
            {
              fprintf(stderr, "Can't open TTF file at '%s', falling back to built-in font.\n", ttf_path);
            }
          }
          if(!ttf_data)
          {
            ttf_data = (u8*)&_binary_data_DejaVuSans_ttf_start;
          }

          if(stbtt_InitFont(&state->font, ttf_data, stbtt_GetFontOffsetForIndex(ttf_data, 0)))
          {
            i32 space_glyph = stbtt_FindGlyphIndex(&state->font, ' ');
            for_count(i, state->custom_glyph_count)
            {
              state->custom_glyphs[i] = space_glyph;
            }

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
                  state->font_char_w, state->font_char_h, state->font_texture_w,
                  state->stb_font_scale, state->stb_font_scale, codepoint);
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

          if(!state->font_texture_id)
          {
            if(ttf_path)
            {
              fprintf(stderr, "Could not generate font from '%s'.\n", ttf_path);
            }
            else
            {
              fprintf(stderr, "Could not generate font.\n");
            }
          }
        }

        {
          char* thread_count_envvar = getenv("I2X_LOADER_THREADS");
          if(thread_count_envvar)
          {
            state->loader_count = atoi(thread_count_envvar);
            state->loader_count = clamp(1, MAX_THREAD_COUNT, state->loader_count);
            printf("Using %d loader thread%s.\n", state->loader_count, state->loader_count == 1 ? "" : "s");
          }

          char* vram_target_mb_envvar = getenv("I2X_TARGET_VRAM_MB");
          if(vram_target_mb_envvar)
          {
            state->shared.total_bytes_limit = (i64)atoi(vram_target_mb_envvar) * 1024 * 1024;
            state->shared.total_bytes_limit = max(0, state->shared.total_bytes_limit);
            printf("Targeting roughly %ld MiB of VRAM usage.\n", state->shared.total_bytes_limit / (1024 * 1024));
          }
        }

        state->shared.total_loader_count = state->loader_count;
        state->shared.img_entries = state->img_entries;
        state->shared.filtered_img_count = state->filtered_img_count;
        state->shared.filtered_img_idxs = state->filtered_img_idxs;

        for(i32 loader_idx = 0;
            loader_idx < state->loader_count;
            ++loader_idx)
        {
          sem_init(&state->loader_semaphores[loader_idx], 0, 0);
          state->loader_data[loader_idx].thread_idx = loader_idx + 1;
          state->loader_data[loader_idx].semaphore = &state->loader_semaphores[loader_idx];
          state->loader_data[loader_idx].shared = &state->shared;
          pthread_create(&state->loader_threads[loader_idx], 0, loader_fun, &state->loader_data[loader_idx]);
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
        b32 border_sampling = true;
        state->linear_sampling = true;
        state->alpha_blend = true;
        b32 bright_bg = false;
        b32 show_fps = false;
        r32 zoom = 0;
        r32 offset_x = 0;
        r32 offset_y = 0;
        state->show_thumbnails = true;
        state->thumbnail_panel_width_ratio = 0.2f;
        state->thumbnail_columns = 2;
        state->scroll_thumbnail_into_view = !!open_single_directory_on.size;
        i32 hovered_thumbnail_idx = -1;
        // state->show_info = 2;
        state->info_panel_width_ratio = 0.2f;

        str_t help_tab_labels[] = {
          str("Keybindings"),
          str("Search"),
        };
        i32 help_tab_count = array_count(help_tab_labels);
        // state->show_help = true;
        // state->help_tab_idx = 1;

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

        ui_interaction_t hovered_interaction = {0};
        ui_interaction_t current_interaction = {0};
        ui_interaction_t mainview_interaction = { &offset_x };
        ui_interaction_t thumbnail_interaction = { &state->viewing_filtered_img_idx };
        ui_interaction_t thumbnail_panel_resize_interaction = { &state->thumbnail_panel_width_ratio };
        ui_interaction_t scrollbar_interaction = { &state->thumbnail_scroll_rows };
        ui_interaction_t info_panel_resize_interaction = { &state->info_panel_width_ratio };

        while(!quitting)
        {
          b32 dirty = false;
          b32 signal_loaders = false;
          b32 need_to_sort = false;
          b32 sort_triggered_by_incomplete_metadata = false;

          {
            shared_loader_data_t* shared = &state->shared;
            i32 uploaded_count = 0;
            i32 deleted_count = 0;

            img_entry_t* unload = state->lru_last;
            while(state->shared.total_bytes_used > state->shared.total_bytes_limit
                && unload)
            {
              img_entry_t* next_unload = unload->lru_prev;
              if(unload != get_filtered_img(state, state->viewing_filtered_img_idx))
              {
                unload_texture(state, unload);
                ++deleted_count;
              }
              unload = next_unload;
            }

            while(shared->next_loaded_img_id > shared->next_finalized_img_id)
            {
              loaded_img_t* loaded_img =
                &shared->loaded_imgs[shared->next_finalized_img_id % array_count(shared->loaded_imgs)];

              if(loaded_img->load_state != LOAD_STATE_LOADED_INTO_RAM)
              {
                // printf("Trying to upload loaded ID %ld (entry %d), but it's not ready yet.\n", shared->next_finalized_img_id, loaded_img->entry_idx);
                break;
              }
              // printf("Uploading loaded ID %ld (entry %d).\n", shared->next_finalized_img_id, loaded_img->entry_idx);

              img_entry_t* img_entry = &state->img_entries[loaded_img->entry_idx];
              if(loaded_img->load_generation == img_entry->load_generation)
              {
                unload_texture(state, img_entry);
                img_entry->w = loaded_img->w;
                img_entry->h = loaded_img->h;
                img_entry->pixels = loaded_img->pixels;
                img_entry->bytes_used = loaded_img->bytes_used;
                img_entry->load_state = LOAD_STATE_LOADED_INTO_RAM;

                if(!img_entry->pixels)
                {
                  img_entry->flags |= IMG_FLAG_FAILED_TO_LOAD;
                }
                else
                {
                  img_entry->flags &= ~IMG_FLAG_FAILED_TO_LOAD;

                  assert(!img_entry->lru_prev);
                  assert(!img_entry->lru_next);

                  // Insert at front of LRU chain so this image doesn't get immediately unloaded.
                  if(!state->lru_first)
                  {
                    state->lru_first = img_entry;
                    state->lru_last = img_entry;
                  }
                  else
                  {
                    img_entry->lru_next = state->lru_first;
                    state->lru_first->lru_prev = img_entry;
                    state->lru_first = img_entry;
                  }
                }
              }
              else
              {
                if(loaded_img->pixels)
                {
                  stbi_image_free(loaded_img->pixels);
                  loaded_img->pixels = 0;
                  __sync_fetch_and_sub(&state->shared.total_bytes_used, loaded_img->bytes_used);
                }
                img_entry->load_state = LOAD_STATE_UNLOADED;
              }
              __sync_synchronize();
              loaded_img->load_state = LOAD_STATE_UNLOADED;

              ++uploaded_count;
              ++shared->next_finalized_img_id;
            }

            if(uploaded_count || deleted_count)
            {
              // TODO: Find something better for unblocking the threads which doesn't
              //       keep going up like these semaphores.
              for_count(i, state->loader_count) { sem_post(&state->loader_semaphores[i]); }
            }
          }

#if 1
          if(state->vsync)
          {
            // TODO: Check for this in the GLX extension string before using it.
            glXDelayBeforeSwapNV(display, glx_window, 0.002f);
          }
#endif

          if(!state->vsync) { dirty = true; }
          if(!state->all_metadata_loaded)
          {
            // printf("Loading metadata %d/%d\n", state->metadata_loaded_count, state->total_img_count);
            dirty = true;
            state->search_changed = true;
            state->need_to_layout = true;

            if(sort_mode_needs_metadata(state->sort_mode))
            {
              need_to_sort = true;
              sort_triggered_by_incomplete_metadata = true;
            }
          }
          state->all_metadata_loaded = (state->metadata_loaded_count >= state->total_img_count);

          if(state->inotify_fd != -1)
          {
            b32 got_notification = false;
            u8 notification_buffer[sizeof(struct inotify_event) + NAME_MAX + 1] = {0};
            ssize_t bytes_available = 0;

            while((bytes_available = read(state->inotify_fd, notification_buffer, sizeof(notification_buffer))) > 0)
            {
              u8* notifications_end = notification_buffer + bytes_available;
              u8* notification_ptr = notification_buffer;
              while(notification_ptr + sizeof(struct inotify_event) <= notifications_end)
              {
                got_notification = true;

                struct inotify_event* notification = (void*)notification_ptr;
                ssize_t notification_size = sizeof(struct inotify_event) + notification->len;

#if 0
                if(notification_ptr == notification_buffer) { printf("\n"); }
                printf("inotify read, %ld / %lu bytes:\n", notification_size, sizeof(notification_buffer));
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
            }

            if(got_notification)
            {
              refresh_input_paths(state);
              signal_loaders = true;
              dirty = true;
            }
          }

          i32 info_height = 0;
          r32 win_min_side = 0;
          r32 fs = 0;
          i32 effective_thumbnail_panel_width = 0;
          i32 effective_info_panel_width = 0;
          i32 image_region_x0 = 0;
          i32 image_region_y0 = 0;
          i32 image_region_w = 0;
          i32 image_region_h = 0;

          while(!quitting)
          {
            win_min_side = min(state->win_w, state->win_h);
            fs = get_font_size(state);
            info_height = 1.2f * fs;
            effective_thumbnail_panel_width = get_effective_thumbnail_panel_width(state);
            effective_info_panel_width = max(1, min(state->win_w - 10, state->win_w * state->info_panel_width_ratio));
            image_region_x0 = effective_thumbnail_panel_width;
            image_region_y0 = (state->show_info == 1) ? info_height : 0;
            image_region_w = max(0, state->win_w - image_region_x0 - (state->show_info == 2 ? effective_info_panel_width : 0));
            image_region_h = max(0, state->win_h - image_region_y0);

            if(!XPending(display)) { break; }

            XEvent event;
            XNextEvent(display, &event);

            // This is needed for XIM to handle combining keys, like ` + a = à.
            if(XFilterEvent(&event, None)) { continue; }

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

                    // TODO: Use XIQueryDevice if the device ID changed since the last input;
                    //       XI_DeviceChanged doesn't seem reliable.

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
                            r32 delta = (*value_ptr - *last) / state->xi_scroll_x_increment;
                            if(absolute(delta) < 5.0f && inside_window)
                            {
                              scroll_x += delta;
                            }
                            *last = *value_ptr;
                          }
                          else if(bit_idx == 3)
                          {
                            r32* last = &state->xi_last_scroll_y_valuator;
                            r32 delta = (*value_ptr - *last) / state->xi_scroll_y_increment;
                            if(absolute(delta) < 5.0f && inside_window)
                            {
                              scroll_y -= delta;
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

                    mouse_delta_x = mouse_x - prev_mouse_x;
                    mouse_delta_y = mouse_y - prev_mouse_y;
                  } break;

#if 0
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
#endif

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
                    if(0) {}
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
                    else if(ctrl_held && keysym == 'q')
                    {
                      quitting = true;
                    }
                    else if(keysym == XK_F1)
                    {
                      bflip(state->show_help);
                    }
                    else if(state->show_help && keysym == XK_Escape)
                    {
                      state->show_help = false;
                    }
                    else if(state->show_help && (shift_held && keysym == XK_Tab))
                    {
                      state->help_tab_idx = i32_wrap_upto(state->help_tab_idx - 1, help_tab_count);
                    }
                    else if(state->show_help && keysym == XK_Tab)
                    {
                      state->help_tab_idx = i32_wrap_upto(state->help_tab_idx + 1, help_tab_count);
                    }
                    else if(ctrl_held && keysym == 'r')
                    {
                      refresh_input_paths(state);
                      signal_loaders = true;
                    }

                    // Debug keys.
                    else if(shift_held && alt_held && keysym == 'a')
                    {
                      bflip(border_sampling);
                    }
                    else if(shift_held && alt_held && keysym == '5')
                    {
                      bflip(show_fps);
                    }
                    else if(shift_held && alt_held && keysym == '6')
                    {
                      bflip(state->alpha_blend);
                    }
                    else if(shift_held && alt_held && keysym == '7')
                    {
                      bflip(state->vsync);
                      glXSwapIntervalEXT(display, glx_window, (int)state->vsync);
                    }
                    else if(shift_held && alt_held && keysym == '8')
                    {
                      bflip(state->debug_font_atlas);
                    }

                    else if(alt_held && keysym == XK_Page_Up)
                    {
                      // TODO: Handle this properly with groups.
                      state->viewing_filtered_img_idx -= state->thumbnail_columns * (r32)(i32)((r32)state->win_h / thumbnail_h);
                      state->viewing_filtered_img_idx = clamp(0, state->filtered_img_count - 1, state->viewing_filtered_img_idx);
                      state->scroll_thumbnail_into_view = true;
                    }
                    else if(alt_held && keysym == XK_Page_Down)
                    {
                      // TODO: Handle this properly with groups.
                      state->viewing_filtered_img_idx += state->thumbnail_columns * (r32)(i32)((r32)state->win_h / thumbnail_h);
                      state->viewing_filtered_img_idx = clamp(0, state->filtered_img_count - 1, state->viewing_filtered_img_idx);
                      state->scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_Page_Up)
                    {
                      state->thumbnail_scroll_rows -= (r32)(i32)((r32)state->win_h / thumbnail_h);
                      clamp_thumbnail_scroll_rows(state);
                    }
                    else if(keysym == XK_Page_Down)
                    {
                      state->thumbnail_scroll_rows += (r32)(i32)((r32)state->win_h / thumbnail_h);
                      clamp_thumbnail_scroll_rows(state);
                    }

                    else if(state->filtering_modal)
                    {
                      if(0) {}
                      else if(keysym == XK_Escape)
                      {
                        state->filtering_modal = false;

                        state->filtered_img_count = state->prev_filtered_img_count;
                        for_count(i, state->filtered_img_count) { state->filtered_img_idxs[i] = state->prev_filtered_img_idxs[i]; }

                        if(state->last_search_history_entry)
                        {
                          str_t last_history_entry = state->last_search_history_entry->str;
                          copy_bytes(last_history_entry.size, last_history_entry.data, state->search_str.data);
                          state->search_str.size = last_history_entry.size;
                        }

                        state->viewing_filtered_img_idx = find_filtered_idx_of_img_idx(state,
                            state->sorted_img_idxs[state->sorted_idx_viewed_before_search]);
                        state->scroll_thumbnail_into_view = true;
                      }
                      else if(keysym == XK_Return || keysym == XK_KP_Enter)
                      {
                        state->filtering_modal = false;

                        if(add_search_history_entry(state, state->search_str) &&
                            state->search_history_file && state->search_str.size > 0)
                        {
                          // Seek to end in case another i2x instance also appended.
                          if(fseek(state->search_history_file, 0, SEEK_END) != -1)
                          {
                            fwrite(state->search_str.data, 1, state->search_str.size, state->search_history_file);
                            fwrite("\n", 1, 1, state->search_history_file);
                            fflush(state->search_history_file);
                          }
                        }
                      }
                      else if(ctrl_held && keysym == 'a')
                      {
                        str_t* str = &state->search_str;
                        state->selection_start = 0;
                        state->selection_end = str->size;
                      }
                      else if(ctrl_held && (keysym == 'c' || keysym == 'x'))
                      {
                        u8* selection_min = state->search_str.data + min(state->selection_start, state->selection_end);
                        u8* selection_max = state->search_str.data + max(state->selection_start, state->selection_end);
                        str_t str = str_from_span(selection_min, selection_max);
                        state->clipboard_str.data = state->clipboard_str_buffer;
                        state->clipboard_str.size = min(sizeof(state->clipboard_str_buffer), STR_SIZE(str));
                        copy_bytes(state->clipboard_str.size, str.data, state->clipboard_str_buffer);
                        if(keysym == 'x')
                        {
                          str_replace_selection(0, &state->search_str,
                              &state->selection_start, &state->selection_end, str(""));
                          state->search_changed = true;
                          state->search_tweaked = true;
                        }
                        XSetSelectionOwner(display, atom_clipboard, window, CurrentTime);
                      }
                      else if(ctrl_held && keysym == 'v')
                      {
                        XConvertSelection(display, atom_clipboard, atom_utf8, atom_mycliptarget, window, CurrentTime);
                      }
                      else if(keysym == XK_BackSpace)
                      {
                        if(state->selection_start == state->selection_end)
                        {
                          state->selection_start = seek_left_in_str(
                              state->search_str, ctrl_held, state->selection_start);
                        }
                        str_replace_selection(0, &state->search_str,
                            &state->selection_start, &state->selection_end, str(""));
                        state->search_changed = true;
                        state->search_tweaked = true;
                      }
                      else if(keysym == XK_Delete)
                      {
                        if(state->selection_start == state->selection_end)
                        {
                          state->selection_end = seek_right_in_str(
                              state->search_str, ctrl_held, state->selection_end);
                        }
                        str_replace_selection(0, &state->search_str,
                            &state->selection_start, &state->selection_end, str(""));
                        state->search_changed = true;
                        state->search_tweaked = true;
                      }
                      else if(keysym == XK_Left)
                      {
                        if(!shift_held && !ctrl_held && state->selection_start != state->selection_end)
                        {
                          state->selection_end = min(state->selection_start, state->selection_end);
                        }
                        else
                        {
                          state->selection_end = seek_left_in_str(state->search_str, ctrl_held, state->selection_end);
                        }

                        if(!shift_held)
                        {
                          state->selection_start = state->selection_end;
                        }
                        state->search_tweaked = true;
                      }
                      else if(keysym == XK_Right)
                      {
                        if(!shift_held && !ctrl_held && state->selection_start != state->selection_end)
                        {
                          state->selection_end = max(state->selection_start, state->selection_end);
                        }
                        else
                        {
                          state->selection_end = seek_right_in_str(state->search_str, ctrl_held, state->selection_end);
                        }

                        if(!shift_held)
                        {
                          state->selection_start = state->selection_end;
                        }
                        state->search_tweaked = true;
                      }
                      else if(keysym == XK_Home)
                      {
                        if(!shift_held)
                        {
                          state->selection_start = 0;
                        }
                        state->selection_end = 0;
                        state->search_tweaked = true;
                      }
                      else if(keysym == XK_End)
                      {
                        if(!shift_held)
                        {
                          state->selection_start = state->search_str.size;
                        }
                        state->selection_end = state->search_str.size;
                        state->search_tweaked = true;
                      }
                      else if(keysym == XK_Up || keysym == XK_Down)
                      {
                        // Search history.
                        if(state->selected_search_history_entry)
                        {
                          b32 going_up = (keysym == XK_Up);
                          str_t search_prefix = {0};
                          if(state->search_tweaked)
                          {
                            search_prefix.data = state->search_str.data;
                            search_prefix.size = state->selection_end;
                          }

                          for(search_history_entry_t* entry =
                                going_up
                                ? state->selected_search_history_entry->prev
                                : state->selected_search_history_entry->next;
                              entry;
                              entry = going_up ? entry->prev : entry->next)
                          {
                            if(entry->str.size > 0)
                            {
                              str_t entry_prefix = { entry->str.data, min(entry->str.size, search_prefix.size) };
                              if(str_eq(entry_prefix, search_prefix) && !str_eq(entry->str, state->search_str))
                              {
                                state->selected_search_history_entry = entry;
                                copy_bytes(entry->str.size, entry->str.data, state->search_str.data);
                                state->search_str.size = entry->str.size;
                                if(!state->search_tweaked)
                                {
                                  state->selection_end = state->search_str.size;
                                }
                                state->selection_start = state->selection_end;
                                state->search_changed = true;
                                break;
                              }
                            }
                          }
                        }
                      }
                      else if(!ctrl_held)
                      {
                        Status xmb_lookup_status = 0;
                        u8 entered_buffer[64];
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

                          if(str_replace_selection(sizeof(state->search_str_buffer), &state->search_str,
                                &state->selection_start, &state->selection_end, entered_str))
                          {
                            state->search_changed = true;
                            state->search_tweaked = true;
                          }
                        }
                      }
                    }
                    else if(!state->sorting_modal && !state->grouping_modal && (ctrl_held && keysym == 'f' || keysym == '/'))
                    {
                      start_search(state);
                    }

                    else if(keysym == XK_Up || keysym == 'k')
                    {
                      if(state->viewing_filtered_img_idx >= 0)
                      {
                        r32 start_y = get_filtered_img(state, state->viewing_filtered_img_idx)->thumbnail_y;
                        state->target_thumbnail_column = min(state->target_thumbnail_column, state->thumbnail_columns - 1);
                        for(i32 filtered_idx = state->viewing_filtered_img_idx - 1;
                            filtered_idx >= 0;
                            --filtered_idx)
                        {
                          img_entry_t* img = get_filtered_img(state, filtered_idx);
                          if(img->thumbnail_column <= state->target_thumbnail_column && img->thumbnail_y != start_y)
                          {
                            state->viewing_filtered_img_idx = filtered_idx;
                            break;
                          }
                        }
                      }
                      state->scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_Down || keysym == 'j')
                    {
                      if(state->viewing_filtered_img_idx >= 0)
                      {
                        r32 start_y = get_filtered_img(state, state->viewing_filtered_img_idx)->thumbnail_y;
                        state->target_thumbnail_column = min(state->target_thumbnail_column, state->thumbnail_columns - 1);
                        i32 row_changes = 0;
                        for(i32 filtered_idx = state->viewing_filtered_img_idx + 1;
                            filtered_idx < state->filtered_img_count;
                            ++filtered_idx)
                        {
                          img_entry_t* img = get_filtered_img(state, filtered_idx);
                          if(img->thumbnail_y != start_y)
                          {
                            ++row_changes;
                            start_y = img->thumbnail_y;
                          }
                          if(row_changes == 1 &&
                              (img->thumbnail_column >= state->target_thumbnail_column
                               || filtered_idx == state->filtered_img_count - 1))
                          {
                            state->viewing_filtered_img_idx = filtered_idx;
                            break;
                          }
                          if(row_changes >= 2)
                          {
                            state->viewing_filtered_img_idx = filtered_idx - 1;
                            break;
                          }
                        }
                      }
                      state->scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_Home || (!shift_held && !ctrl_held && keysym == 'g'))
                    {
                      state->viewing_filtered_img_idx = 0;
                      state->target_thumbnail_column =
                        state->img_entries[state->filtered_img_idxs[state->viewing_filtered_img_idx]].thumbnail_column;
                      state->scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == XK_End || (shift_held && !ctrl_held && keysym == 'g'))
                    {
                      state->viewing_filtered_img_idx = max(0, state->filtered_img_count - 1);
                      state->target_thumbnail_column =
                        state->img_entries[state->filtered_img_idxs[state->viewing_filtered_img_idx]].thumbnail_column;
                      state->scroll_thumbnail_into_view = true;
                    }

                    else if(state->sorting_modal)
                    {
                      if(0) {}
                      else if(keysym == XK_Escape)
                      {
                        state->sorting_modal = false;

                        for_count(i, state->sorted_img_count) { state->sorted_img_idxs[i] = state->prev_sorted_img_idxs[i]; }
                        for_count(i, state->filtered_img_count) { state->filtered_img_idxs[i] = state->prev_filtered_img_idxs[i]; }
                        state->viewing_filtered_img_idx = state->filtered_idx_viewed_before_sort;
                        state->scroll_thumbnail_into_view = true;
                        state->sort_mode = state->prev_sort_mode;
                        state->sort_descending = state->prev_sort_descending;
                        state->need_to_layout = true;
                      }
                      else if(keysym == XK_Return || keysym == XK_KP_Enter)
                      {
                        state->sorting_modal = false;
                      }
                      else if(keysym == XK_Left)
                      {
                        if(state->sort_mode > 0)
                        {
                          --state->sort_mode;
                        }
                        else
                        {
                          state->sort_mode = SORT_MODE_COUNT - 1;
                        }
                        need_to_sort = true;
                      }
                      else if(keysym == XK_Right)
                      {
                        ++state->sort_mode;
                        if(state->sort_mode >= SORT_MODE_COUNT)
                        {
                          state->sort_mode = 0;
                        }
                        need_to_sort = true;
                      }
                      else if(keysym == 'd')
                      {
                        bflip(state->sort_descending);
                        need_to_sort = true;
                      }
                      else if(keysym == 'f')
                      {
                        state->sort_mode = SORT_MODE_FILEPATH;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                      else if(keysym == 't')
                      {
                        state->sort_mode = SORT_MODE_TIMESTAMP;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                      else if(keysym == 's')
                      {
                        state->sort_mode = SORT_MODE_FILESIZE;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                      else if(keysym == 'o')
                      {
                        state->sort_mode = SORT_MODE_RANDOM;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                      else if(keysym == 'x')
                      {
                        state->sort_mode = SORT_MODE_PIXELCOUNT;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                      else if(keysym == 'p')
                      {
                        state->sort_mode = SORT_MODE_PROMPT;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                      else if(keysym == 'm')
                      {
                        state->sort_mode = SORT_MODE_MODEL;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                      else if(keysym == 'r')
                      {
                        state->sort_mode = SORT_MODE_SCORE;
                        state->sort_descending = shift_held;
                        need_to_sort = true;
                      }
                    }
                    else if(!state->grouping_modal && shift_held && keysym == 's')
                    {
                      state->sorting_modal = true;

                      for_count(i, state->sorted_img_count) { state->prev_sorted_img_idxs[i] = state->sorted_img_idxs[i]; }
                      for_count(i, state->filtered_img_count) { state->prev_filtered_img_idxs[i] = state->filtered_img_idxs[i]; }
                      state->filtered_idx_viewed_before_sort = state->viewing_filtered_img_idx;
                      state->prev_sort_mode = state->sort_mode;
                      state->prev_sort_descending = state->sort_descending;
                    }

                    else if(state->grouping_modal)
                    {
                      if(0) {}
                      else if(keysym == XK_Escape)
                      {
                        state->grouping_modal = false;
                        state->group_mode = state->prev_group_mode;
                        state->scroll_thumbnail_into_view = true;
                      }
                      else if(keysym == XK_Return || keysym == XK_KP_Enter)
                      {
                        state->grouping_modal = false;
                      }
                      else if(keysym == XK_Left)
                      {
                        if(state->group_mode > 0)
                        {
                          --state->group_mode;
                        }
                        else
                        {
                          state->group_mode = GROUP_MODE_COUNT - 1;
                        }
                        state->scroll_thumbnail_into_view = true;
                      }
                      else if(keysym == XK_Right)
                      {
                        ++state->group_mode;
                        if(state->group_mode >= GROUP_MODE_COUNT)
                        {
                          state->group_mode = 0;
                        }
                        state->scroll_thumbnail_into_view = true;
                      }
                      else if(keysym == 'o')
                      {
                        state->group_mode = GROUP_MODE_NONE;
                        state->scroll_thumbnail_into_view = true;
                      }
                      else if(keysym == 'd')
                      {
                        state->group_mode = GROUP_MODE_DAY;
                        state->scroll_thumbnail_into_view = true;
                      }
                      else if(keysym == 'p')
                      {
                        state->group_mode = GROUP_MODE_PROMPT;
                        state->scroll_thumbnail_into_view = true;
                      }
                      else if(keysym == 'm')
                      {
                        state->group_mode = GROUP_MODE_MODEL;
                        state->scroll_thumbnail_into_view = true;
                      }
                    }
                    else if(ctrl_held && keysym == 'g')
                    {
                      state->grouping_modal = true;
                      state->prev_group_mode = state->group_mode;
                    }

                    else if(keysym == XK_Escape)
                    {
                      // TODO: Better to persist the marking-state.
                      b32 any_marked = false;
                      for_count(i, state->total_img_count) { any_marked = any_marked || (state->img_entries[i].flags & IMG_FLAG_MARKED); }
                      if(!any_marked) { quitting = true; }
                    }
                    else if(keysym == XK_BackSpace || keysym == XK_Left || keysym == 'h')
                    {
                      if(state->viewing_filtered_img_idx > 0)
                      {
                        state->viewing_filtered_img_idx -= 1;
                        state->target_thumbnail_column = get_filtered_img(state, state->viewing_filtered_img_idx)->thumbnail_column;
                      }
                      state->scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == ' ' || keysym == XK_Right || keysym == 'l')
                    {
                      if(state->viewing_filtered_img_idx < state->filtered_img_count - 1)
                      {
                        state->viewing_filtered_img_idx += 1;
                        state->target_thumbnail_column = get_filtered_img(state, state->viewing_filtered_img_idx)->thumbnail_column;
                      }
                      state->scroll_thumbnail_into_view = true;
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
                        state->scroll_thumbnail_into_view = true;
                      }
                    }
                    else if(keysym == 'o')
                    {
                      i32 prev_sorted_idx = find_sorted_idx_of_img_idx(state,
                          state->filtered_img_idxs[state->viewing_filtered_img_idx]);

                      if(state->filtered_img_count == state->sorted_img_count)
                      {
                        b32 some_marked = false;
                        for_count(i, state->total_img_count)
                        {
                          if(state->img_entries[i].flags & IMG_FLAG_MARKED)
                          {
                            some_marked = true;
                            break;
                          }
                        }

                        if(some_marked)
                        {
                          state->viewing_filtered_img_idx = 0;
                          state->filtered_img_count = 0;
                          for_count(sorted_idx, state->sorted_img_count)
                          {
                            i32 img_idx = state->sorted_img_idxs[sorted_idx];

                            if(state->img_entries[img_idx].flags & IMG_FLAG_MARKED)
                            {
                              if(prev_sorted_idx >= sorted_idx)
                              {
                                state->viewing_filtered_img_idx = state->filtered_img_count;
                              }

                              state->filtered_img_idxs[state->filtered_img_count++] = img_idx;
                            }
                          }
                        }
                      }
                      else
                      {
                        reset_filtered_images(state);
                        state->viewing_filtered_img_idx = find_filtered_idx_of_img_idx(state,
                            state->sorted_img_idxs[prev_sorted_idx]);
                      }
                      clamp_thumbnail_scroll_rows(state);
                      state->scroll_thumbnail_into_view = true;
                    }
                    else if(keysym == 'b')
                    {
                      bflip(bright_bg);
                    }
                    else if(keysym == 't')
                    {
                      bflip(state->show_thumbnails);
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
                          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        }
                        else
                        {
                          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
                          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
                        }
                      }
                    }
#if 0
                    else if(ctrl_held && keysym == 'v')
                    {
                      XConvertSelection(display, atom_clipboard, atom_uri_list, atom_mycliptarget, window, CurrentTime);
                    }
#endif
                    else if(ctrl_held && keysym == 'c')
                    {
                      // TODO: Copy list of marked images if there are any.
                      if(state->filtered_img_count)
                      {
                        img_entry_t* img = get_filtered_img(state, state->viewing_filtered_img_idx);
                        if(shift_held)
                        {
                          state->clipboard_str = img->parameter_strings[IMG_STR_POSITIVE_PROMPT];
                        }
                        else
                        {
                          state->clipboard_str = img->path;
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
                        clamp_thumbnail_scroll_rows(state);
                        state->scroll_thumbnail_into_view = true;
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
                        clamp_thumbnail_scroll_rows(state);
                        state->scroll_thumbnail_into_view = true;
                      }
                      else
                      {
                        zoom += 0.25f;
                      }
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

                  if(xi_available)
                  {
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
                    if(state->filtering_modal)
                    {
                      str_replace_selection(sizeof(state->search_str_buffer),
                          &state->search_str, &state->selection_start, &state->selection_end,
                          str_from_start_and_size(data, item_count));
                      state->search_changed = true;
                    }
                    else
                    {
                      printf("Paste: format: %d, count: %lu, bytes left: %lu, data: %s\n",
                          format, item_count, bytes_left, data);
                      // TODO: Parse URL-encoded file://... path and open it.
                    }
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

                  if(request->property != None && state->clipboard_str.data != 0)
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
                      for(i32 sorted_idx = 0;
                          sorted_idx < state->total_img_count;
                          ++sorted_idx)
                      {
                        i32 img_idx = state->sorted_img_idxs[sorted_idx];

                        char* path = 0;
                        if(any_marked)
                        {
                          if(!(state->img_entries[img_idx].flags & IMG_FLAG_MARKED)) { continue; }
                          path = (char*)state->img_entries[img_idx].path.data;
                        }
                        else
                        {
                          path = (char*)state->clipboard_str.data;
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
                          state->clipboard_str.data, state->clipboard_str.size);

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
                state->scroll_thumbnail_into_view = true;
                dirty = true;
              }
              else if(mouse_btn_went_up == 2)
              {
                state->viewing_filtered_img_idx -= 1;
                state->scroll_thumbnail_into_view = true;
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

            b32 mouse_on_thumbnail_panel_edge = false;
            b32 mouse_in_thumbnail_panel = false;
            b32 mouse_on_scrollbar = false;
            b32 mouse_on_info_panel_edge = false;
            b32 mouse_in_info_panel = false;
            if(state->show_thumbnails
                && mouse_x >= effective_thumbnail_panel_width - get_scrollbar_width(state)
                && mouse_x < effective_thumbnail_panel_width)
            {
              mouse_on_scrollbar = true;
            }
            else if(state->show_thumbnails
                && mouse_x >= effective_thumbnail_panel_width
                && mouse_x < effective_thumbnail_panel_width + 10)
            {
              mouse_on_thumbnail_panel_edge = true;
            }
            else if(state->show_thumbnails && !mouse_on_thumbnail_panel_edge && mouse_x < effective_thumbnail_panel_width)
            {
              mouse_in_thumbnail_panel = true;
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
                state->dragging_start_value2 = state->thumbnail_scroll_rows;
              }
              else if(mouse_on_thumbnail_panel_edge)
              {
                hovered_interaction = thumbnail_panel_resize_interaction;
                state->dragging_start_value = effective_thumbnail_panel_width;
                state->dragging_start_value2 = state->thumbnail_scroll_rows;
              }
              else if(mouse_in_thumbnail_panel)
              {
                hovered_interaction = thumbnail_interaction;

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

            if(interaction_eq(current_interaction, thumbnail_interaction) && lmb_held)
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
                state->target_thumbnail_column = get_filtered_img(state, state->viewing_filtered_img_idx)->thumbnail_column;
                // Do not scroll the newly selected thumbnail into view!
                // It would mess with the extra-row padding.
              }
              dirty = true;
            }
            else if(interaction_eq(current_interaction, scrollbar_interaction))
            {
              r32 thumbnail_rows = get_thumbnail_rows(state);
              if(mmb_held)
              {
                state->thumbnail_scroll_rows = (state->win_h - mouse_y) * thumbnail_rows / state->win_h;
              }
              else if(lmb_held)
              {
                state->thumbnail_scroll_rows -= mouse_delta_y * thumbnail_rows / state->win_h;
              }
              clamp_thumbnail_scroll_rows(state);
              dirty = true;
            }
            else if(interaction_eq(current_interaction, thumbnail_panel_resize_interaction))
            {
              if(lmb_held)
              {
                if(state->win_w != 0)
                {
                  state->thumbnail_panel_width_ratio =
                    (r32)(state->dragging_start_value + (i32)(mouse_x - state->dragging_start_x))
                    / state->win_w;
                  state->thumbnail_scroll_rows = state->dragging_start_value2;
                  clamp_thumbnail_scroll_rows(state);
                  dirty = true;

                  // TODO: Keep a visible image fixed.
                  state->scroll_thumbnail_into_view = true;
                }
              }
            }
            else if(interaction_eq(current_interaction, info_panel_resize_interaction))
            {
              if(lmb_held)
              {
                if(state->win_w != 0)
                {
                  state->info_panel_width_ratio =
                    (r32)(state->dragging_start_value - (i32)(mouse_x - state->dragging_start_x))
                    / state->win_w;
                  state->info_panel_width_ratio = clamp(0, 1, state->info_panel_width_ratio);
                  dirty = true;
                }
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
                  state->scroll_thumbnail_into_view = true;
                }
              }

              if(!alt_held)
              {
                if(mouse_in_thumbnail_panel || mouse_on_scrollbar)
                {
                  if(ctrl_held)
                  {
                    if(scroll_y_ticks != 0)
                    {
                      img_entry_t* prev_visible_img = get_filtered_img(state, hovered_thumbnail_idx);
                      r32 y_threshold = mouse_y - state->win_h - state->thumbnail_scroll_rows * thumbnail_h;
                      if(!prev_visible_img)
                      {
                        // Find image on a row near the mouse.
                        prev_visible_img = &state->img_entries[0];
                        for(i32 i = 0;
                            i < state->filtered_img_count;
                            ++i)
                        {
                          img_entry_t* img = &state->img_entries[state->filtered_img_idxs[i]];
                          r32 y = img->thumbnail_y;
                          if(y > y_threshold)
                          {
                            prev_visible_img = img;
                          }
                          else
                          {
                            break;
                          }
                        }
                      }
                      r32 prev_visible_top_y = prev_visible_img->thumbnail_y + state->thumbnail_scroll_rows * thumbnail_h;

                      state->thumbnail_columns -= scroll_y_ticks;
                      clamp_thumbnail_columns(state);
                      group_and_layout_thumbnails(state);
                      thumbnail_h = get_thumbnail_size(state);

                      r32 new_visible_top_y = prev_visible_img->thumbnail_y + state->thumbnail_scroll_rows * thumbnail_h;
                      state->thumbnail_scroll_rows += (prev_visible_top_y - new_visible_top_y) / thumbnail_h;
                      clamp_thumbnail_scroll_rows(state);
                    }
                  }
                  else if(shift_held)
                  {
                  }
                  else
                  {
                    state->thumbnail_scroll_rows -= scroll_y;
                    clamp_thumbnail_scroll_rows(state);
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

          if(quitting) { break; }

          if(need_to_sort)
          {
            if(state->sort_mode == SORT_MODE_RANDOM)
            {
              for_count(i, state->total_img_count)
              {
                state->img_entries[i].random_number = max(1, (u32)rand());
              }
            }

            i32 prev_img_idx_viewed = state->filtered_img_idxs[state->viewing_filtered_img_idx];

            qsort_r(state->sorted_img_idxs, state->sorted_img_count, sizeof(state->sorted_img_idxs[0]), compare_img_entries, state);
            qsort_r(state->filtered_img_idxs, state->filtered_img_count, sizeof(state->filtered_img_idxs[0]), compare_img_entries, state);

            if(sort_triggered_by_incomplete_metadata)
            {
              state->viewing_filtered_img_idx = find_filtered_idx_of_img_idx(state, prev_img_idx_viewed);
            }
            else
            {
              state->viewing_filtered_img_idx = 0;
            }

            state->scroll_thumbnail_into_view = true;
            state->need_to_layout = true;
            dirty = true;
            signal_loaders = true;
          }

          // Search.
          if(state->filtering_modal && state->search_changed)
          {
            if(state->search_str.size == 0)
            {
              reset_filtered_images(state);
              state->viewing_filtered_img_idx = find_filtered_idx_of_img_idx(state,
                  state->sorted_img_idxs[state->sorted_idx_viewed_before_search]);
            }
            else
            {
              // i64 nsecs_search_start = get_nanoseconds();
              // 24ms before change for "the quick brown fox jumps over the lazy dog" on ~/downloads/ComfyUI/output, optimized.
              // 27.5ms after change, bitset for non-excluded matches.
              // 24ms again if bitset is sparse with excluded matches.
              // 20.5ms with SEARCH_MATCHED flag.
              // 19.5ms with search_tasks loop.
              // 23ms with alternatives and flexible model search.
              // 18ms with 64-bit bloom filter on possible first byte.
              // 12.5ms with 64-bit bloom filter on possible first two bytes.
              // 9ms with 64-bit bloom filter on possible first three bytes.

              state->filtered_img_count = 0;
              state->viewing_filtered_img_idx = 0;

              str_t query = state->search_str;
              u8* query_end = query.data + query.size;

              enum
              {
                SEARCH_MATCHED = (1 << 0),
                SEARCH_EXCLUDE = (1 << 1),
              };
              typedef u32 search_flags_t;

              typedef struct search_item_t
              {
                union
                {
                  str_t word;
                  struct
                  {
                    r32 min_r32;
                    r32 max_r32;
                  };
                };
                search_flags_t flags;

                struct search_item_t* next;
                struct search_item_t* next_alternative;
              } search_item_t;

              search_item_t search_items[256];
              i32 next_search_item_idx = 0;
              search_item_t* first_path_item = 0;
              search_item_t* first_model_item = 0;
              search_item_t* first_positive_item = 0;
              search_item_t* first_negative_item = 0;
              search_item_t* first_width_item = 0;
              search_item_t* first_height_item = 0;
              search_item_t* first_pixelcount_item = 0;
              search_item_t* first_aspect_item = 0;
              search_item_t* first_steps_item = 0;
              search_item_t* first_cfg_item = 0;
              search_item_t* first_score_item = 0;
              search_item_t* first_age_h_item = 0;

              // TODO: Separate bloom filter for each search task.
              u64 bloom = 0;

              for(u8 *word_start = query.data, *word_end = query.data;
                  word_start < query_end && next_search_item_idx < array_count(search_items);
                 )
              {
                // TODO: Consider "quoted strings" as one word.  Or maybe use {braces}?
                while(word_start < query_end && *word_start == ' ')
                {
                  ++word_start;
                }

                b32 exclude = false;
                if(*word_start == '-')
                {
                  exclude = true;
                  ++word_start;
                }

                search_item_t* last_alternative = 0;
                b32 is_r32 = false;
                do
                {
                  word_end = word_start;

                  u8* column_at = 0;
                  while(word_end < query_end && *word_end != ' ' && *word_end != '|')
                  {
                    if(!column_at && *word_end == ':')
                    {
                      column_at = word_end;
                    }

                    ++word_end;
                  }

                  if(word_end > word_start)
                  {
                    search_item_t* item = &search_items[next_search_item_idx++];
                    zero_struct(*item);
                    if(exclude) { item->flags |= SEARCH_EXCLUDE; }
                    if(last_alternative)
                    {
                      if(!is_r32)
                      {
                        item->word = str_from_span(word_start, word_end);
                        last_alternative->next_alternative = item;
                      }
                      // TODO: Handle number range alternatives.
                    }
                    else
                    {
                      str_t pre_column = {0};
                      str_t post_column = {0};
                      if(column_at)
                      {
                        pre_column = str_from_span(word_start, column_at);
                        post_column = str_from_span(column_at + 1, word_end);
                      }

                      struct
                      {
                        str_t key;
                        search_item_t** first_item_ptr;
                        b32 is_r32;
                      } search_keywords[] = {
                        { str("f"), &first_path_item },
                        { str("m"), &first_model_item },
                        { str("p"), &first_positive_item },
                        { str("n"), &first_negative_item },
                        { str("width"), &first_width_item, true },
                        { str("height"), &first_height_item, true },
                        { str("pixelcount"), &first_pixelcount_item, true },
                        { str("aspect"), &first_aspect_item, true },
                        { str("steps"), &first_steps_item, true },
                        { str("cfg"), &first_cfg_item, true },
                        { str("score"), &first_score_item, true },
                        { str("age_h"), &first_age_h_item, true },
                      };

                      b32 keyword_found = false;

                      for_count(keyword_idx, array_count(search_keywords))
                      {
                        search_item_t** first_item_ptr = search_keywords[keyword_idx].first_item_ptr;
                        if(!search_keywords[keyword_idx].is_r32)
                        {
                          if(str_eq(pre_column, search_keywords[keyword_idx].key))
                          {
                            keyword_found = true;
                            item->word = post_column;
                            item->next = *first_item_ptr;
                            *first_item_ptr = item;
                          }
                        }
                        else
                        {
                          if(str_eq(pre_column, search_keywords[keyword_idx].key) && post_column.size >= 2)
                          {
                            keyword_found = true;
                            is_r32 = true;
                            b32 inequality = true;
                            b32 did_arithmetic = false;
                            u8* numbers_ptr = post_column.data + 1;
                            u8* numbers_end = post_column.data + post_column.size;
                            if(post_column.data[1] == '=')
                            {
                              ++numbers_ptr;
                              inequality = false;
                            }
                            r64 parsed_r64 = parse_next_r64(&numbers_ptr, numbers_end);
                            while(numbers_ptr + 2 <= numbers_end)
                            {
                              if(*numbers_ptr == '*' || *numbers_ptr == 'x')
                              {
                                ++numbers_ptr;
                                r64 next_num = parse_next_r64(&numbers_ptr, numbers_end);
                                parsed_r64 *= next_num;
                                did_arithmetic = true;
                              }
                              else if(*numbers_ptr == '/')
                              {
                                ++numbers_ptr;
                                r64 next_num = parse_next_r64(&numbers_ptr, numbers_end);
                                parsed_r64 /= next_num;
                                did_arithmetic = true;
                              }
                              else
                              {
                                break;
                              }
                            }
                            r32 parsed = (r32)parsed_r64;

                            b32 valid_item = true;
                            if(post_column.data[0] == '=' || post_column.data[0] == '!')
                            {
                              item->min_r32 = did_arithmetic ? 0.999f * parsed : parsed;
                              item->max_r32 = did_arithmetic ? 1.001f * parsed : parsed;
                              if(post_column.data[0] == '!') { item->flags |= SEARCH_EXCLUDE; }
                            }
                            else if(post_column.data[0] == '~')
                            {
                              item->min_r32 = 0.9f * parsed;
                              item->max_r32 = 1.1f * parsed;
                            }
                            else if(post_column.data[0] == '>')
                            {
                              item->min_r32 = inequality ? nextafterf(parsed, R32_MAX) : parsed;
                              item->max_r32 = R32_MAX;
                            }
                            else if(post_column.data[0] == '<')
                            {
                              item->min_r32 = R32_MIN;
                              item->max_r32 = inequality ? nextafterf(parsed, R32_MIN) : parsed;
                            }
                            else
                            {
                              valid_item = false;
                            }

                            if(valid_item)
                            {
                              item->next = *first_item_ptr;
                              *first_item_ptr = item;
                            }
                          }
                        }
                      }

                      if(!keyword_found)
                      {
                        item->word = str_from_span(word_start, word_end);
                        item->next = first_positive_item;
                        first_positive_item = item;
                      }
                    }

                    if(!is_r32)
                    {
                      if(item->word.size > 0)
                      {
                        // The upper-case region of ASCII maps into the control-code region
                        // when taking off the two highest bits;  use this to avoid collisions
                        // with the symbols and digits, which would get mapped from lower-case.
                        bloom |= (1 << (to_upper(item->word.data[0]) >> 2));
                      }
                      else
                      {
                        bloom |= ~0;
                      }
                    }

                    last_alternative = item;
                  }

                  word_start = word_end;
                  if(*word_end == '|') { ++word_start; }
                } while(*word_end == '|');
              }

#if 0
              {
                struct
                {
                  char* name;
                  search_item_t* first_item;
                } searches[] = {
                  { "model", first_model_item },
                  { "positive", first_positive_item },
                  { "negative", first_negative_item },
                  { "path", first_path_item },
                };
                for_count(i, array_count(searches))
                {
                  if(searches[i].first_item)
                  {
                    printf("\n%s:\n", searches[i].name);
                    for(search_item_t* item = searches[i].first_item;
                        item;
                        item = item->next)
                    {
                      printf("  %s\"%.*s\"", item->flags & SEARCH_EXCLUDE ? "EXCLUDE " : "", PF_STR(item->word));
                      for(search_item_t* alt = item->next_alternative;
                          alt;
                          alt = alt->next_alternative)
                      {
                        printf(" | \"%.*s\"", PF_STR(alt->word));
                      }
                      printf("\n");
                    }
                  }
                }
              }
#endif

              struct timespec ts_now;
              clock_gettime(CLOCK_REALTIME, &ts_now);

              for_count(sorted_idx, state->sorted_img_count)
              {
                i32 img_idx = state->sorted_img_idxs[sorted_idx];
                img_entry_t* img = &state->img_entries[img_idx];
                b32 overall_match = true;

                // String search.
                {
                  struct
                  {
                    str_t haystack;
                    search_item_t* first_item;
                  } search_tasks[] = {
                    { img->path, first_path_item },
                    { img->parameter_strings[IMG_STR_MODEL], first_model_item },
                    { img->parameter_strings[IMG_STR_POSITIVE_PROMPT], first_positive_item },
                    { img->parameter_strings[IMG_STR_NEGATIVE_PROMPT], first_negative_item },
                  };
                  for(i32 search_task_idx = 0;
                      search_task_idx < array_count(search_tasks);
                      ++search_task_idx)
                  {
                    str_t haystack = search_tasks[search_task_idx].haystack;
                    search_item_t* first_item = search_tasks[search_task_idx].first_item;
                    if(first_item)
                    {
                      for(search_item_t* item = first_item;
                          item;
                          item = item->next)
                      {
                        item->flags &= ~SEARCH_MATCHED;
                      }

                      for(i64 offset = 0;
                          offset < (i64)haystack.size && overall_match;
                          ++offset)
                      {
                        if(!(bloom & (1 << (to_upper(haystack.data[offset]) >> 2)))) { continue; }

                        for(search_item_t* item = first_item;
                            item;
                            item = item->next)
                        {
                          if(!(item->flags & SEARCH_MATCHED))
                          {
                            for(search_item_t* alternative = item;
                                alternative;
                                alternative = alternative->next_alternative)
                            {
                              str_t query_word = alternative->word;

                              if(haystack.size >= query_word.size + offset)
                              {
                                // TODO: If multiple query word prefixes match, pick the longest one eagerly.
                                str_t prompt_substr = { haystack.data + offset, query_word.size };
                                if(str_eq_ignoring_case(prompt_substr, query_word))
                                {
                                  // TODO: Think about whether to consider already-matched words
                                  //       earlier in the prompt for exclusion.
                                  if(item->flags & SEARCH_EXCLUDE)
                                  {
                                    overall_match = false;
                                  }
                                  else
                                  {
                                    item->flags |= SEARCH_MATCHED;
                                  }
                                  goto _search_end_label;
                                }
                              }
                            }
                          }
                        }
_search_end_label:
                        offset = offset;  // Dummy expression to avoid compiler warning.
                      }

                      for(search_item_t* item = first_item;
                          item;
                          item = item->next)
                      {
                        if(!(item->flags & SEARCH_EXCLUDE))
                        {
                          overall_match = overall_match && (item->flags & SEARCH_MATCHED);
                        }
                      }
                    }
                  }
                }

                // Numeric search.
                {
                  struct
                  {
                    b32 img_has_value;
                    r32 img_r32;
                    search_item_t* first_item;
                  } search_tasks[] = {
                    { true, (r32)img->w, first_width_item },
                    { true, (r32)img->h, first_height_item },
                    { true, (r32)(img->w * img->h), first_pixelcount_item },
                    { true, img->h == 0 ? 0 : (r32)img->w / (r32)img->h, first_aspect_item },
                    { true, (r32)(ts_now.tv_sec - img->modified_at_time.tv_sec) / 3600.0f, first_age_h_item },
                    { (img->parameter_strings[IMG_STR_SAMPLING_STEPS].size > 0), img->parsed_r32s[PARSED_R32_SAMPLING_STEPS], first_steps_item },
                    { (img->parameter_strings[IMG_STR_CFG].size > 0), img->parsed_r32s[PARSED_R32_CFG], first_cfg_item },
                    { (img->parameter_strings[IMG_STR_SCORE].size > 0), img->parsed_r32s[PARSED_R32_SCORE], first_score_item },
                  };
                  for(i32 search_task_idx = 0;
                      search_task_idx < array_count(search_tasks);
                      ++search_task_idx)
                  {
                    search_item_t* first_item = search_tasks[search_task_idx].first_item;
                    if(first_item)
                    {
                      if(!search_tasks[search_task_idx].img_has_value)
                      {
                        overall_match = false;
                      }
                      else
                      {
                        r32 img_r32 = search_tasks[search_task_idx].img_r32;
                        for(search_item_t* item = first_item;
                            item;
                            item = item->next)
                        {
                          b32 matches = ((img_r32 >= item->min_r32) && (img_r32 <= item->max_r32));
                          b32 should_match = ((item->flags & SEARCH_EXCLUDE) == 0);
                          if(matches != should_match)
                          {
                            overall_match = false;
                          }
                        }
                      }
                    }
                  }
                }

                if(overall_match)
                {
#if 1
                  if(state->sorted_idx_viewed_before_search >= sorted_idx)
                  {
                    state->viewing_filtered_img_idx = state->filtered_img_count;
                  }
#endif

                  state->filtered_img_idxs[state->filtered_img_count++] = img_idx;
                }
              }

              // i64 nsecs_search_end = get_nanoseconds();
              // r64 msecs = 1e-6 * (nsecs_search_end - nsecs_search_start);
              // printf("%.3f ms for \"%.*s\"\n", msecs, PF_STR(state->search_str));
            }

            dirty = true;
            state->scroll_thumbnail_into_view = true;
            state->search_changed = false;
            state->need_to_layout = true;
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
            img_entry_t* viewed_img = &dummy_img;
            if(state->filtered_img_count > 0)
            {
              viewing_img_idx = state->filtered_img_idxs[state->viewing_filtered_img_idx];
              viewed_img = &state->img_entries[viewing_img_idx];
            }

            if(last_viewing_img_idx != viewing_img_idx)
            {
              char txt[256];
              txt[0] = 0;
              char* path = (char*)viewed_img->path.data;
              if(path)
              {
                i32 txt_len = snprintf(txt, sizeof(txt), "%s - %s", PROGRAM_NAME, path);
                set_title(display, window, (u8*)txt, txt_len);
              }
              else
              {
                set_title(display, window, (unsigned char*)PROGRAM_NAME, sizeof(PROGRAM_NAME) - 1);
              }

              last_viewing_img_idx = viewing_img_idx;
            }

            group_and_layout_thumbnails(state);

            if(state->scroll_thumbnail_into_view)
            {
#if 1
              r32 extra_rows = 0.25f * state->win_h / thumbnail_h;
              r32 thumbnail_row = -viewed_img->thumbnail_y / thumbnail_h;
              state->thumbnail_scroll_rows = min(thumbnail_row,
                  clamp(
                    thumbnail_row + 1 - state->win_h / thumbnail_h + extra_rows,
                    thumbnail_row - extra_rows,
                    state->thumbnail_scroll_rows));
#else
              state->thumbnail_scroll_rows = (-viewed_img->thumbnail_y - 0.5f * state->win_h) / thumbnail_h + 0.5f;
#endif

              clamp_thumbnail_scroll_rows(state);
              state->scroll_thumbnail_into_view = false;
            }

            i32 first_visible_thumbnail_idx = max(0, state->filtered_img_count - 1);
            i32 last_visible_thumbnail_idx = -1;

            for(i32 filtered_idx = 0;
                filtered_idx < state->filtered_img_count;
                ++filtered_idx)
            {
              img_entry_t* img = &state->img_entries[state->filtered_img_idxs[filtered_idx]];
              r32 y_top = img->thumbnail_y + state->win_h + state->thumbnail_scroll_rows * thumbnail_h;

              if(filtered_idx < first_visible_thumbnail_idx && y_top - thumbnail_h <= state->win_h)
              {
                first_visible_thumbnail_idx = filtered_idx;
              }

              if(y_top + 2 * fs >= 0)
              {
                last_visible_thumbnail_idx = filtered_idx;
              }
              else
              {
                break;
              }
            }

            if(0
                || state->viewing_filtered_img_idx != state->shared.viewing_filtered_img_idx
                || first_visible_thumbnail_idx != state->shared.first_visible_thumbnail_idx
                || last_visible_thumbnail_idx != state->shared.last_visible_thumbnail_idx
                || state->filtered_img_count != state->shared.filtered_img_count
                || signal_loaders
              )
            {
              state->shared.viewing_filtered_img_idx = state->viewing_filtered_img_idx;
              state->shared.first_visible_thumbnail_idx = first_visible_thumbnail_idx;
              state->shared.last_visible_thumbnail_idx = last_visible_thumbnail_idx;
              state->shared.filtered_img_count = state->filtered_img_count;
              for_count(i, state->loader_count) { sem_post(&state->loader_semaphores[i]); }
            }

            glViewport(0, 0, state->win_w, state->win_h);
            glDisable(GL_SCISSOR_TEST);
            {
              r32 bg_gray = bright_bg ? 0.9f : 0.1f;
              glClearColor(bg_gray, bg_gray, bg_gray, 1.0f);
            }
            glClear(GL_COLOR_BUFFER_BIT);
            glEnable(GL_SCISSOR_TEST);
            glEnable(GL_TEXTURE_2D);

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

            r32 text_gray = bright_bg ? 0 : 1;
            r32 label_gray = bright_bg ? 0.3f : 0.7f;
            r32 highlight_gray = bright_bg ? 0.7f : 0.3f;
            r32 background_gray = bright_bg ? 1 : 0;

            b32 still_loading = false;

            // Draw main image.
            {
              still_loading |= upload_img_texture(state, viewed_img);

              glColor3f(1.0f, 1.0f, 1.0f);
              GLuint texture_id = viewed_img->texture_id;
              r32 tex_w = viewed_img->w;
              r32 tex_h = viewed_img->h;
              if(viewed_img->flags & IMG_FLAG_FAILED_TO_LOAD)
              {
                texture_id = 0;
              }
              if(state->debug_font_atlas)
              {
                // FONT TEST
                texture_id = state->font_texture_id;
                tex_w = state->font_texture_w;
                tex_h = state->font_texture_h;
              }

              if(texture_id)
              {
                if(state->alpha_blend)
                {
                  if(texture_id == state->font_texture_id)
                  {
                    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                  }
                  else
                  {
                    glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                  }
                  glEnable(GL_BLEND);
                }
                else
                {
                  glDisable(GL_BLEND);
                }

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

                glBegin(GL_QUADS);
                glTexCoord2f(u0, v1); glVertex2f(x0, y0);
                glTexCoord2f(u1, v1); glVertex2f(x1, y0);
                glTexCoord2f(u1, v0); glVertex2f(x1, y1);
                glTexCoord2f(u0, v0); glVertex2f(x0, y1);
                glEnd();
              }

              if(state->show_info == 1)
              {
                glScissor(image_region_x0, 0, state->win_w - image_region_x0, info_height);
                // glScissor(0, 0, state->win_w, state->win_h);
                glColor3f(text_gray, text_gray, text_gray);

                r32 x = image_region_x0 + 0.2f * fs;
                r32 y = fs * (state->font_descent + 0.1f);
                str_t str = viewed_img->parameter_strings[IMG_STR_POSITIVE_PROMPT];
                draw_str(state, 0, fs, x, y, str);
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

                u8 tmp[256];
                str_t tmp_str = { tmp };

#define SHOW_LABEL_VALUE(label, value) \
                if(value.size > 0) \
                { \
                  y -= fs; \
                  x = x0; \
                  glColor3f(label_gray, label_gray, label_gray); \
                  x += draw_str(state, 0, fs, x, y, str(label)); \
                  glColor3f(text_gray, text_gray, text_gray); \
                  draw_wrapped_text(state, fs, x_indented, x1, &x, &y, value); \
                }

                if(state->filtered_img_count == state->sorted_img_count)
                {
                  tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%d/%d",
                      state->viewing_filtered_img_idx + 1, state->filtered_img_count);
                }
                else
                {
                  tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%d/%d of %d total",
                      state->viewing_filtered_img_idx + 1, state->filtered_img_count, state->sorted_img_count);
                }
                SHOW_LABEL_VALUE("", tmp_str);

                SHOW_LABEL_VALUE("File: ", viewed_img->path);
                {
                  struct tm t = {0};
                  localtime_r(&viewed_img->modified_at_time.tv_sec, &t);
                  tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%04d-%02d-%02d %02d:%02d:%02d",
                      t.tm_year + 1900, t.tm_mon + 1, t.tm_mday,
                      t.tm_hour, t.tm_min, t.tm_sec);
                }
                SHOW_LABEL_VALUE("Time: ", tmp_str);
                if(viewed_img->filesize < 10000)
                {
                  tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%lu B", viewed_img->filesize);
                }
                else if(viewed_img->filesize < 10000000)
                {
                  tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%.0f kB", 1e-3 * (r64)viewed_img->filesize);
                }
                else
                {
                  tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%.0f MB", 1e-6 * (r64)viewed_img->filesize);
                }
                SHOW_LABEL_VALUE("Size: ", tmp_str);
                if(viewed_img->w || viewed_img->h)
                {
                  tmp_str.size = snprintf((char*)tmp, sizeof(tmp), "%dx%d", viewed_img->w, viewed_img->h);
                  SHOW_LABEL_VALUE("Resolution: ", tmp_str);
                }
                else
                {
                  y -= fs;
                }

                y -= fs;
                if(viewed_img->metadata_generation != viewed_img->load_generation &&
                    state->metadata_loaded_count <= viewing_img_idx)
                {
                  SHOW_LABEL_VALUE("Loading metadata...", str(" "));
                }
                else
                {
                  SHOW_LABEL_VALUE("Model: ", viewed_img->parameter_strings[IMG_STR_MODEL]);
                  SHOW_LABEL_VALUE("Sampler: ", viewed_img->parameter_strings[IMG_STR_SAMPLER]);
                  SHOW_LABEL_VALUE("Sampling steps: ", viewed_img->parameter_strings[IMG_STR_SAMPLING_STEPS]);
                  SHOW_LABEL_VALUE("CFG: ", viewed_img->parameter_strings[IMG_STR_CFG]);
                  SHOW_LABEL_VALUE("Batch size: ", viewed_img->parameter_strings[IMG_STR_BATCH_SIZE]);
                  SHOW_LABEL_VALUE("Seed: ", viewed_img->parameter_strings[IMG_STR_SEED]);
                  SHOW_LABEL_VALUE("Positive prompt: ", viewed_img->parameter_strings[IMG_STR_POSITIVE_PROMPT]);
                  SHOW_LABEL_VALUE("Negative prompt: ", viewed_img->parameter_strings[IMG_STR_NEGATIVE_PROMPT]);
                  SHOW_LABEL_VALUE("Score: ", viewed_img->parameter_strings[IMG_STR_SCORE]);

#if 0
                  y -= fs;
                  SHOW_LABEL_VALUE("All parameters: ", viewed_img->parameter_strings[IMG_STR_GENERATION_PARAMETERS]);
#endif
                }
#undef SHOW_LABEL_VALUE
              }
            }

            if(state->show_thumbnails)
            {
              glScissor(0, 0, effective_thumbnail_panel_width, state->win_h);

              glDisable(GL_BLEND);
              glBindTexture(GL_TEXTURE_2D, 0);

              glBegin(GL_QUADS);

              // Scrollbar.
              i32 scrollbar_width = get_scrollbar_width(state);
              {
                r32 scrollbar_edge_gray = 0.5f;
                if(interaction_eq(hovered_interaction, scrollbar_interaction))
                {
                  scrollbar_edge_gray = bright_bg ? 0.3f : 0.7f;
                }
                glColor3f(scrollbar_edge_gray, scrollbar_edge_gray, scrollbar_edge_gray);

                r32 thumbnail_rows = get_thumbnail_rows(state);
                r32 scrollbar_top_ratio = state->thumbnail_scroll_rows / (r32)thumbnail_rows;
                r32 scrollbar_bottom_ratio = (state->thumbnail_scroll_rows + state->win_h / thumbnail_h) / (r32)thumbnail_rows;
                i32 scrollbar_yrange = max(2, (i32)(state->win_h * (scrollbar_bottom_ratio - scrollbar_top_ratio) + 0.5f));
                i32 scrollbar_y1 = (i32)(state->win_h * (1 - scrollbar_top_ratio) + 0.5f);
                i32 scrollbar_y0 = scrollbar_y1 - scrollbar_yrange;

                glVertex2f(effective_thumbnail_panel_width - scrollbar_width, scrollbar_y0);
                glVertex2f(effective_thumbnail_panel_width - 2, scrollbar_y0);
                glVertex2f(effective_thumbnail_panel_width - 2, scrollbar_y1);
                glVertex2f(effective_thumbnail_panel_width - scrollbar_width, scrollbar_y1);
              }

              // Resizing bar.
              r32 thumbnail_panel_edge_gray = 0.0f;
              if(interaction_eq(hovered_interaction, thumbnail_panel_resize_interaction))
              {
                thumbnail_panel_edge_gray = bright_bg ? 0.0f : 1.0f;
              }
              else
              {
                thumbnail_panel_edge_gray = bright_bg ? 0.4f : 0.6f;
              }

              glColor3f(thumbnail_panel_edge_gray, thumbnail_panel_edge_gray, thumbnail_panel_edge_gray);
              glVertex2f(effective_thumbnail_panel_width - 2, 0);
              glVertex2f(effective_thumbnail_panel_width - 1, 0);
              glVertex2f(effective_thumbnail_panel_width - 1, state->win_h);
              glVertex2f(effective_thumbnail_panel_width - 2, state->win_h);

              glEnd();

              if(state->alpha_blend)
              {
                glEnable(GL_BLEND);
              }
              else
              {
                glDisable(GL_BLEND);
              }

              glColor3f(1.0f, 1.0f, 1.0f);

              glScissor(0, 0, max(0, effective_thumbnail_panel_width - scrollbar_width), state->win_h);
              hovered_thumbnail_idx = -1;

              // Traverse the thumbnails backwards to update the LRU chain
              // in a way that avoids flickering loading/unloading.
              for(i32 filtered_idx = last_visible_thumbnail_idx;
                  filtered_idx >= first_visible_thumbnail_idx;
                  --filtered_idx)
              {
                img_entry_t* img = get_filtered_img(state, filtered_idx);
                still_loading |= upload_img_texture(state, img);

                r32 box_x0 = img->thumbnail_column * thumbnail_w;
                r32 box_y1 = img->thumbnail_y + state->win_h + state->thumbnail_scroll_rows * thumbnail_h;
                r32 box_x1 = box_x0 + thumbnail_w;
                r32 box_y0 = box_y1 - thumbnail_h;

                if(state->group_mode != GROUP_MODE_NONE &&
                    (filtered_idx == 0 || get_filtered_img(state, filtered_idx - 1)->thumbnail_group != img->thumbnail_group))
                {
                  u8 tmp[256];
                  str_t labels[2] = {0};
                  i32 label_count = 1;

                  switch(state->group_mode)
                  {
                    case GROUP_MODE_DAY:
                    {
                      struct tm t = {0};
                      localtime_r(&img->modified_at_time.tv_sec, &t);
                      labels[0].data = tmp;
                      labels[0].size = snprintf((char*)tmp, sizeof(tmp),
                          "%04d-%02d-%02d", t.tm_year + 1900, t.tm_mon + 1, t.tm_mday);
                    } break;

                    case GROUP_MODE_PROMPT:
                    {
                      labels[0] = img->parameter_strings[IMG_STR_POSITIVE_PROMPT];
                      if(img->parameter_strings[IMG_STR_NEGATIVE_PROMPT].size > 0)
                      {
                        label_count = 2;
                        labels[1].data = tmp;
                        labels[1].size = snprintf((char*)tmp, sizeof(tmp),
                            "- %.*s", PF_STR(img->parameter_strings[IMG_STR_NEGATIVE_PROMPT]));
                      }
                    } break;

                    case GROUP_MODE_MODEL:
                    {
                      labels[0] = img->parameter_strings[IMG_STR_MODEL];
                    } break;
                  }

                  r32 label_x0 = box_x0 + 0.15f * fs;
                  r32 label_y0 = box_y1;
                  r32 label_y1 = box_y1 + (label_count + 0.25f) * fs;

                  if(prev_mouse_x < effective_thumbnail_panel_width - scrollbar_width
                      && prev_mouse_y >= label_y0
                      && prev_mouse_y < label_y1)
                  {
                    glDisable(GL_SCISSOR_TEST);

                    r32 x1 = 0;
                    for_count(i, label_count)
                    {
                      x1 = max(x1, draw_str(state, DRAW_STR_MEASURE_ONLY, fs, 0, 0, labels[i]) + 0.5f * fs);
                    }

                    glBindTexture(GL_TEXTURE_2D, 0);
                    r32 g = bright_bg ? 0.9f : 0.1f;
                    glColor3f(g, g, g);
                    glBegin(GL_QUADS);
                    glVertex2f(0, label_y0);
                    glVertex2f(x1, label_y0);
                    glVertex2f(x1, label_y1);
                    glVertex2f(0, label_y1);
                    glEnd();
                  }

                  glColor3f(text_gray, text_gray, text_gray);
                  for_count(i, label_count)
                  {
                    r32 y = label_y0 + fs * ((label_count - i - 1) + 1.5f * state->font_descent);
                    // TODO: Wrap text if longer than window width.
                    // TODO: Highlight changes between previous and next groups.
                    draw_str(state, 0, fs, label_x0, y, labels[i]);
                  }

                  glEnable(GL_SCISSOR_TEST);
                }

                if(hovered_thumbnail_idx == -1 &&
                    interaction_eq(hovered_interaction, thumbnail_interaction) &&
                    max(0, prev_mouse_x) >= box_x0 &&
                    (prev_mouse_x < box_x1 || img->thumbnail_column == state->thumbnail_columns - 1) &&
                    prev_mouse_y >= box_y0 &&
                    prev_mouse_y < box_y1
                  )
                {
                  hovered_thumbnail_idx = filtered_idx;
                }

                if(filtered_idx == state->viewing_filtered_img_idx || filtered_idx == hovered_thumbnail_idx)
                {
                  glBindTexture(GL_TEXTURE_2D, 0);

                  b32 viewing_this = (filtered_idx == state->viewing_filtered_img_idx);
                  r32 gray = viewing_this ? (bright_bg ? 0.1f : 0.9f) : 0.5f;

                  // TODO: Clamp these adjusted coordinates to screen pixels
                  //       to make it look nicer at small sizes.
                  r32 corner = 0.2f * thumbnail_h;
                  glColor3f(gray, gray, gray);
                  glBegin(GL_TRIANGLE_FAN);
                  glVertex2f(0.5f * (box_x0 + box_x1), 0.5f * (box_y0 + box_y1));
                  glVertex2f(box_x0 + corner, box_y0);
                  glVertex2f(box_x1 - corner, box_y0);
                  glVertex2f(box_x1,          box_y0 + corner);
                  glVertex2f(box_x1,          box_y1 - corner);
                  glVertex2f(box_x1 - corner, box_y1);
                  glVertex2f(box_x0 + corner, box_y1);
                  glVertex2f(box_x0,          box_y1 - corner);
                  glVertex2f(box_x0,          box_y0 + corner);
                  glVertex2f(box_x0 + corner, box_y0);
                  glEnd();

                  if(viewing_this)
                  {
                    r32 inset_x = 0.04f * thumbnail_w;
                    r32 inset_y = 0.04f * thumbnail_h;
                    glColor3f(1 - gray, 1 - gray, 1 - gray);
                    glBegin(GL_QUADS);
                    glVertex2f(box_x0 + inset_x, box_y0 + inset_y);
                    glVertex2f(box_x1 - inset_x, box_y0 + inset_y);
                    glVertex2f(box_x1 - inset_x, box_y1 - inset_y);
                    glVertex2f(box_x0 + inset_x, box_y1 - inset_y);
                    glEnd();
                  }
                }

                GLuint texture_id = 0;
                r32 tex_w = img->w;
                r32 tex_h = img->h;
                if(!(img->flags & IMG_FLAG_FAILED_TO_LOAD))
                {
                  texture_id = img->texture_id;
                }

                if(texture_id)
                {
                  glBindTexture(GL_TEXTURE_2D, texture_id);
                  glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);
                  glColor3f(1.0f, 1.0f, 1.0f);

                  r32 mag = 1.0f;
                  if(tex_w != 0 && tex_h != 0)
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
                else
                {
                  str_t msg = (img->flags & IMG_FLAG_FAILED_TO_LOAD) ? str("Unsupported") : str("...");
                  r32 unscaled_msg_width = draw_str(state, DRAW_STR_MEASURE_ONLY, 1, 0, 0, msg);
                  r32 msg_scale = min(2 * fs, 0.9f * thumbnail_w / max(1.0f, unscaled_msg_width));
                  r32 x = 0.5f * (box_x0 + box_x1 - msg_scale * unscaled_msg_width);
                  r32 y = 0.5f * (box_y0 + box_y1 - msg_scale * state->font_ascent);
                  glColor3f(text_gray, text_gray, text_gray);
                  draw_str(state, 0, msg_scale, x, y, msg);
                }

                if(img->flags & IMG_FLAG_MARKED)
                {
                  r32 tag_scale = min(2 * fs, 0.4f * min(thumbnail_w, thumbnail_h));
                  r32 x = lerp(box_x0, box_x1, 0.05f);
                  r32 y = lerp(box_y0, box_y1, 0.95f) - tag_scale * state->font_ascent;
                  glColor3f(0, 0, 0);
                  draw_str(state, 0, tag_scale, x + 0.05f * tag_scale, y - 0.05f * tag_scale, str("M"));
                  glColor3f(0, 1, 0);
                  draw_str(state, 0, tag_scale, x, y, str("M"));
                }
              }
            }

            if(state->filtering_modal)
            {
              str_t text = state->search_str;
              u8* selection_min = text.data + min(state->selection_start, state->selection_end);
              u8* selection_max = text.data + max(state->selection_start, state->selection_end);

              glScissor(0, 0, state->win_w, state->win_h);
              r32 min_box_width = 10 * fs;
              r32 x1 = state->win_w - 0.6f * fs;
              r32 x0 = max(0, min(image_region_x0 + 0.5f * fs, x1 - min_box_width));
              r32 y1 = state->win_h - 1.5f * fs;
              r32 x_indented = x0 + fs;
              str_t label = str("Search: ");

              // Background rectangle / progress bar.
              {
                r32 x = x0;
                r32 y = y1;

                x += draw_str(state, DRAW_STR_MEASURE_ONLY, fs, x, y, label);
                wrapped_text_ctx_t wrap_ctx = begin_wrapped_text(state, fs, x_indented, x1, text);
                do
                {
                  wrap_next_line(&wrap_ctx, x);
                } while(finish_wrapped_line(&wrap_ctx, &x, &y));

                r32 loading_gray = bright_bg ? 0.8f : 0.2f;
                r32 x_max = (wrap_ctx.line_idx == 0) ? wrap_ctx.line_end_x : x1;
                r32 box_x0 = x0 - 0.3f * fs;
                r32 box_x1 = x_max + 0.3f * fs;
                r32 box_y0 = y - fs * (state->font_descent + 0.2f);
                r32 box_y1 = y1 + fs * (state->font_ascent + 0.2f);

                r32 x_progress_split = box_x1;
                if(state->metadata_loaded_count < state->total_img_count)
                {
                  r32 completion_ratio = (r32)state->metadata_loaded_count / (r32)state->total_img_count;
                  x_progress_split = lerp(box_x0, box_x1, completion_ratio);
                }

                glBindTexture(GL_TEXTURE_2D, 0);
                glDisable(GL_BLEND);
                glBegin(GL_QUADS);
                glColor3f(background_gray, background_gray, background_gray);
                glVertex2f(box_x0, box_y0);
                glVertex2f(x_progress_split, box_y0);
                glVertex2f(x_progress_split, box_y1);
                glVertex2f(box_x0, box_y1);
                if(x_progress_split != box_x1)
                {
                  glColor3f(loading_gray, loading_gray, loading_gray);
                  glVertex2f(x_progress_split, box_y0);
                  glVertex2f(box_x1, box_y0);
                  glVertex2f(box_x1, box_y1);
                  glVertex2f(x_progress_split, box_y1);
                }
                glEnd();
              }

              {
                r32 x = x0;
                r32 y = y1;

                glColor3f(label_gray, label_gray, label_gray);
                x += draw_str(state, 0, fs, x, y, label);

                wrapped_text_ctx_t wrap_ctx = begin_wrapped_text(state, fs, x_indented, x1, text);

                glColor3f(text_gray, text_gray, text_gray);
                b32 cursor_found = false;
                r32 cursor_x = x;
                r32 cursor_y = y;
                b32 selection_range_started = false;
                do
                {
                  str_t line = wrap_next_line(&wrap_ctx, x);
                  u8* line_end = line.data + line.size;

                  u8* selection_min_in_line = clamp(line.data, line_end, selection_min);
                  u8* selection_max_in_line = clamp(line.data, line_end, selection_max);

                  str_t span_before_selection = str_from_span(line.data, selection_min_in_line);
                  str_t span_in_selection = str_from_span(selection_min_in_line, selection_max_in_line);
                  str_t span_after_selection = str_from_span(selection_max_in_line, line_end);

                  i32 last_glyph = 0;
                  x += draw_str_advanced(state, 0, 1, fs, x, y, span_before_selection, &last_glyph);

                  r32 highlight_x0 = x;
                  r32 highlight_x1 = x;
                  if(selection_min < wrap_ctx.remaining_text.data && selection_max > line_end)
                  {
                    highlight_x1 = wrap_ctx.line_end_x + 0.5f * fs;
                  }
                  else
                  {
                    i32 scratch_glyph = last_glyph;
                    highlight_x1 += draw_str_advanced(state, DRAW_STR_MEASURE_ONLY,
                        1, fs, x, y, span_in_selection, &scratch_glyph);
                  }
                  if(highlight_x0 != highlight_x1)
                  {
                    glColor3f(highlight_gray, highlight_gray, highlight_gray);
                    glBindTexture(GL_TEXTURE_2D, 0);
                    glDisable(GL_BLEND);
                    glBegin(GL_QUADS);
                    glVertex2f(highlight_x0, y - fs * state->font_descent);
                    glVertex2f(highlight_x1, y - fs * state->font_descent);
                    glVertex2f(highlight_x1, y + fs * state->font_ascent);
                    glVertex2f(highlight_x0, y + fs * state->font_ascent);
                    glEnd();
                    glColor3f(text_gray, text_gray, text_gray);
                  }
                  x += draw_str_advanced(state, 0, 1, fs, x, y, span_in_selection, &last_glyph);

                  x += draw_str_advanced(state, 0, 1, fs, x, y, span_after_selection, &last_glyph);

                  if(!cursor_found && text.data + state->selection_end <= line_end)
                  {
                    if(state->selection_end < state->selection_start)
                    {
                      cursor_x = highlight_x0;
                    }
                    else
                    {
                      cursor_x = highlight_x1;
                    }
                    cursor_y = y;
                    cursor_found = true;
                  }
                } while(finish_wrapped_line(&wrap_ctx, &x, &y));

                // Draw cursor.
                glBindTexture(GL_TEXTURE_2D, 0);
                glDisable(GL_BLEND);
                glBegin(GL_QUADS);
                glVertex2f(cursor_x - 0.5f, cursor_y - fs * state->font_descent);
                glVertex2f(cursor_x + 0.5f, cursor_y - fs * state->font_descent);
                glVertex2f(cursor_x + 0.5f, cursor_y + fs * state->font_ascent);
                glVertex2f(cursor_x - 0.5f, cursor_y + fs * state->font_ascent);
                glEnd();
              }
            }

            if(state->sorting_modal)
            {
              glScissor(0, 0, state->win_w, state->win_h);
              r32 box_width = 36 * fs;
              r32 x1 = state->win_w - 0.6f * fs;
              r32 x0 = max(0, min(image_region_x0 + 0.5f * fs, x1 - box_width));
              r32 y1 = state->win_h - 1.5f * fs;
              r32 x_indented = x0 + fs;

              // Background rectangle.
              {
                r32 x = x0;
                r32 y = y1;

                r32 box_x0 = x0 - 0.3f * fs;
                r32 box_x1 = x0 + box_width;
                r32 box_y0 = y - fs * (state->font_descent + 2.2f);
                r32 box_y1 = y1 + fs * (state->font_ascent + 0.2f);

                glBindTexture(GL_TEXTURE_2D, 0);
                glDisable(GL_BLEND);
                glBegin(GL_QUADS);
                glColor3f(background_gray, background_gray, background_gray);
                glVertex2f(box_x0, box_y0);
                glVertex2f(box_x1, box_y0);
                glVertex2f(box_x1, box_y1);
                glVertex2f(box_x0, box_y1);
                glEnd();
              }

              {
                r32 x = x0;
                r32 y = y1;

                glColor3f(label_gray, label_gray, label_gray);
                x += draw_str(state, 0, fs, x, y, str("Sort by (hold Shift for descending):"));

                y -= fs;
                x = x_indented;
                for_count(mode_idx, SORT_MODE_COUNT)
                {
                  x += 0.3 * fs;
                  if(mode_idx == state->sort_mode)
                  {
                    glColor3f(text_gray, text_gray, text_gray);
                  }
                  else
                  {
                    glColor3f(label_gray, label_gray, label_gray);
                  }
                  x += draw_str(state, 0, fs, x, y, sort_mode_labels[mode_idx]);
                }

                y -= fs;
                x = x_indented;
                if(!state->sort_descending)
                {
                  glColor3f(text_gray, text_gray, text_gray);
                }
                else
                {
                  glColor3f(label_gray, label_gray, label_gray);
                }
                x += draw_str(state, 0, fs, x, y, str("ascending"));
                x += 0.3 * fs;
                if(state->sort_descending)
                {
                  glColor3f(text_gray, text_gray, text_gray);
                }
                else
                {
                  glColor3f(label_gray, label_gray, label_gray);
                }
                x += draw_str(state, 0, fs, x, y, str("[d]escending"));
              }
            }

            if(state->grouping_modal)
            {
              glScissor(0, 0, state->win_w, state->win_h);
              r32 box_width = 36 * fs;
              r32 x1 = state->win_w - 0.6f * fs;
              r32 x0 = max(0, min(image_region_x0 + 0.5f * fs, x1 - box_width));
              r32 y1 = state->win_h - 1.5f * fs;
              r32 x_indented = x0 + fs;

              // Background rectangle.
              {
                r32 x = x0;
                r32 y = y1;

                r32 box_x0 = x0 - 0.3f * fs;
                r32 box_x1 = x0 + box_width;
                r32 box_y0 = y - fs * (state->font_descent + 1.2f);
                r32 box_y1 = y1 + fs * (state->font_ascent + 0.2f);

                glBindTexture(GL_TEXTURE_2D, 0);
                glDisable(GL_BLEND);
                glBegin(GL_QUADS);
                glColor3f(background_gray, background_gray, background_gray);
                glVertex2f(box_x0, box_y0);
                glVertex2f(box_x1, box_y0);
                glVertex2f(box_x1, box_y1);
                glVertex2f(box_x0, box_y1);
                glEnd();
              }

              {
                r32 x = x0;
                r32 y = y1;

                glColor3f(label_gray, label_gray, label_gray);
                x += draw_str(state, 0, fs, x, y, str("Group by:"));

                y -= fs;
                x = x_indented;
                for_count(mode_idx, GROUP_MODE_COUNT)
                {
                  x += 0.3 * fs;
                  if(mode_idx == state->group_mode)
                  {
                    glColor3f(text_gray, text_gray, text_gray);
                  }
                  else
                  {
                    glColor3f(label_gray, label_gray, label_gray);
                  }
                  x += draw_str(state, 0, fs, x, y, group_mode_labels[mode_idx]);
                }
              }
            }

            if(state->show_help)
            {
              r32 box_width = min(state->win_w, 45 * fs);
              r32 box_x0 = 0.5f * (state->win_w - box_width);
              r32 box_x1 = 0.5f * (state->win_w + box_width);
              r32 box_y0 = 0;
              r32 box_y1 = min(state->win_h, 34 * fs);

              glScissor(box_x0, box_y0, box_x1 - box_x0, box_y1 - box_y0);

              glEnable(GL_BLEND);
              glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
              glColor4f(background_gray, background_gray, background_gray, 0.85f);
              glBindTexture(GL_TEXTURE_2D, 0);
              glBegin(GL_QUADS);
              glVertex2f(box_x0, box_y0);
              glVertex2f(box_x1, box_y0);
              glVertex2f(box_x1, box_y1);
              glVertex2f(box_x0, box_y1);
              glEnd();
              glDisable(GL_BLEND);

              r32 x0 = box_x0 + 0.5f * fs;
              r32 x1 = box_x1 - 0.5f * fs;
              r32 x = x0;
              r32 y = box_y1 - 1.25f * fs;

              for(i32 tab_idx = 0;
                  tab_idx < help_tab_count;
                  ++tab_idx)
              {
                b32 active_tab = (state->help_tab_idx == tab_idx);

                if(active_tab)
                {
                  glColor3f(text_gray, text_gray, text_gray);
                }
                else
                {
                  glColor3f(label_gray, label_gray, label_gray);
                }

                x += draw_str(state, 0, fs, x, y, help_tab_labels[tab_idx]);
                x += fs;
              }

              glColor3f(label_gray, label_gray, label_gray);
              x += fs;
              x += draw_str(state, 0, fs, x, y, str("(Tab ↹ for next section, F1 to toggle this help.)"));

              glBindTexture(GL_TEXTURE_2D, 0);
              glBegin(GL_QUADS);
              glVertex2f(box_x0, y - fs * (0.25f + state->font_descent) - 1);
              glVertex2f(box_x1, y - fs * (0.25f + state->font_descent) - 1);
              glVertex2f(box_x1, y - fs * (0.25f + state->font_descent));
              glVertex2f(box_x0, y - fs * (0.25f + state->font_descent));
              glEnd();

              x = x0;
              y -= 1.0f * fs;
              r32 x_indented = x0 + fs;
              r32 x_column = x0 + 16 * fs;

#define SHOW_LABEL_VALUE(label, binding) \
              { \
                y -= fs; \
                x = x0; \
                \
                glColor3f(label_gray, label_gray, label_gray); \
                r32 label_width = draw_str(state, DRAW_STR_MEASURE_ONLY, fs, x, y, str(label)); \
                x = x_column - label_width - 1.0f * fs; \
                draw_str(state, 0, fs, x, y, str(label)); \
                \
                glColor3f(text_gray, text_gray, text_gray); \
                x = x_column; \
                x += draw_str(state, 0, fs, x, y, str(binding)); \
              }

              r32 ypad = 0.5f * fs;
              if(0) {}
              else if(state->help_tab_idx == 0)
              {
                // Key help.

                SHOW_LABEL_VALUE("Quit", "Ctrl + Q");
                SHOW_LABEL_VALUE("Navigate images", "Space/Backspace, Arrows, HJKL, LMB/MMB, Alt + Scroll");
                SHOW_LABEL_VALUE("Jump to first/last", "Home/End, G / Shift + G");
                y -= ypad;
                SHOW_LABEL_VALUE("Pan image", "LMB-Drag");
                SHOW_LABEL_VALUE("Zoom image", "Scroll, 0/-/=, MMB-Drag, Ctrl + LMB-Drag");
                SHOW_LABEL_VALUE("Zoom to fit", "X");
                SHOW_LABEL_VALUE("Zoom to 1:1", "Z, 1");
                SHOW_LABEL_VALUE("Zoom to 2:1, 3:1, 4:1", "2/3/4");
                SHOW_LABEL_VALUE("Zoom to 1:2, 1:3, 1:4", "Shift + 2/3/4");
                y -= ypad;
                SHOW_LABEL_VALUE("Toggle info bar/panel", "I");
                SHOW_LABEL_VALUE("Toggle thumbnails", "T");
                SHOW_LABEL_VALUE("Change thumbnail column count", "Alt + 0/-/=, Ctrl + Scroll on thumbnails");
                SHOW_LABEL_VALUE("Navigate one page up/down", "Alt + PgUp/PgDn");
                y -= ypad;
                SHOW_LABEL_VALUE("Sort", "Shift + S");
                SHOW_LABEL_VALUE("Search", "/, Ctrl + F");
                SHOW_LABEL_VALUE("Group", "Ctrl + G");
                SHOW_LABEL_VALUE("Mark images", "M, Ctrl + A, Ctrl/Shift + LMB on thumbnails");
                SHOW_LABEL_VALUE("Mark image and go to next", "Shift + M");
                SHOW_LABEL_VALUE("Copy current or marked images", "Ctrl + C");
                SHOW_LABEL_VALUE("Show only marked images", "O");
                y -= ypad;
                SHOW_LABEL_VALUE("Toggle nearest-pixel filtering", "N");
                SHOW_LABEL_VALUE("Toggle bright/dark mode", "B");
                SHOW_LABEL_VALUE("Copy positive prompt (WIP)", "Shift + Ctrl + C (might not work if there are marked images)");
                SHOW_LABEL_VALUE("Refresh images", "Ctrl + R");
              }
              else if(state->help_tab_idx == 1)
              {
                // Search help.

                y -= fs;
                x = x0;
                glColor3f(text_gray, text_gray, text_gray);
                draw_wrapped_text(state, fs, x0, x1, &x, &y, str(
                      "When the search box is open (Ctrl+F or /), images can be filtered by prompt and other metadata.\n"
                      "Everything is case-insensitive and mostly order-independent.\n"
                      "\n"
                      "EXAMPLE:  blue -jay sketch|paint\n"
                      "This will match positive prompts that include \"blue\" and "
                      "either \"sketch\" or \"paint\", but not \"jay\".\n"
                      "It would accept \"sketch of blue sky\", \"Painting a Blue Sketch\", but NOT "
                      "\"blue car\" or \"blue jay sketch\".\n"
                      "\n"
                      "Additional parameters can be specified with these special prefixes:\n"
                      "  f:<file path>  m:<model name>  n:<negative prompt>\n"
                      "  width:<op><w>  height:<op><h>  pixelcount:<op><w*h>  aspect:<op><w/h>\n"
                      "  age_h:<op><hours>\n"
                      "  steps:<op><sampling steps>  cfg:<op><CFG>  score:<op><score>\n"
                      "\n"
                      "EXAMPLE:  m:sd -f:bad|tmp m:0.9\n"
                      "This will match images created with a model that includes both \"sd\" and \"0.9\" "
                      "(e.g. \"SDXL_0.9vae\", but neither \"sd_xl_1.0\" nor \"v1-5-pruned\"), "
                      "but only if their filepath does NOT include \"bad\" or \"tmp\" (so \"good/pic.png\" is OK, but \"tmp/pic.png\" is excluded).\n"
                      "\n"
                      "Numeric values can be compared with an <op> including <, <=, =, >=, >, ~= (±10%), !=.\n"
                      "EXAMPLE:  width:>=500 width:<600 -cfg:=7\n"
                      "This will match images with a width between 500 inclusive and 600 exclusive, "
                      "but only if their CFG value is known and does not equal 7.\n"
                      "Simple multiplications and divisions get evaluated, e.g. aspect:=16/9 pixelcount:>64*64.\n"
                      "Alternatives (e.g. width:<500|>600) are NOT supported for numbers.\n"
                      "\n"
                      "The search is accepted with Enter or canceled with Escape. "
                      "History is available via Up/Down.\n"
                      "If image metadata are still getting loaded (e.g. from a slow filesystem), "
                      "the search box turns into a progress bar. "
                      "While it is not full, the search results may be incomplete."
                      ));
              }
            }
#undef SHOW_LABEL_VALUE

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
