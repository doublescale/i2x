// Defines sigaction on msys:
#ifndef _GNU_SOURCE
#define _GNU_SOURCE
#endif

#include <assert.h>
#include <inttypes.h>
#include <math.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <time.h>
#include <unistd.h>

// https://www.x.org/releases/current/doc/libX11/libX11/libX11.html
// TODO: Only define what's necessary.
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

#include "lib/stb_image.h"

#include "util.h"

#define WINDOW_INIT_W 800
#define WINDOW_INIT_H 600
#define PROGRAM_NAME "i2x"

// static void (*glXSwapIntervalEXT)(Display *dpy, GLXDrawable drawable, int interval);

typedef struct
{
  char* path;
  GLuint texture_id;
  i32 w;
  i32 h;
  u8* pixels;
} img_entry_t;

internal u64 get_nanoseconds()
{
  struct timespec ts;
  clock_gettime(CLOCK_MONOTONIC, &ts);
  return ts.tv_nsec + 1000000000 * ts.tv_sec;
}

internal void set_title(Display* display, Window window, u8* txt, i32 txt_len)
{
  XChangeProperty(display, window, XA_WM_NAME, XA_STRING, 8, PropModeReplace, txt, txt_len);
}

internal void update_scroll_increments(i32 class_count, XIAnyClassInfo** classes,
    i32 scroll_increment_count, r32* scroll_increment_by_source_id)
{
  // printf("  classes:\n");
  for(i32 class_idx = 0;
      class_idx < class_count;
      ++class_idx)
  {
    XIAnyClassInfo* class = classes[class_idx];
#if 0
    printf("    type: %d, sourceid: %d\n", class->type, class->sourceid);
    if(class->type == XIValuatorClass)
    {
      XIValuatorClassInfo* valuator_class = (XIValuatorClassInfo*)class;
      printf("    ValuatorClass\n");
      printf("      number: %d\n", valuator_class->number);
      printf("      min: %f\n", valuator_class->min);
      printf("      max: %f\n", valuator_class->max);
      printf("      value: %f\n", valuator_class->value);
    }
#endif
    if(class->type == XIScrollClass)
    {
      XIScrollClassInfo* scroll_class = (XIScrollClassInfo*)class;
#if 0
      printf("    ScrollClass\n");
      printf("      number: %d\n", scroll_class->number);
      printf("      scroll_type: %d\n", scroll_class->scroll_type);
      printf("      increment: %f\n", scroll_class->increment);
      printf("      flags: %d\n", scroll_class->flags);
#endif

      if(class->sourceid >= 0 && class->sourceid < scroll_increment_count)
      {
        if(scroll_increment_by_source_id[class->sourceid] != scroll_class->increment)
        {
          printf("Scroll increment for sourceid %d changed from %f to %f.\n",
              class->sourceid,
              scroll_increment_by_source_id[class->sourceid],
              scroll_class->increment);
          scroll_increment_by_source_id[class->sourceid] = scroll_class->increment;
        }
      }
    }
  }
}

int main(int argc, char** argv)
{
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

        b32 vsync = true;

        // TODO: Check if GLX_EXT_swap_control is in the extensions string first.
        // Enable VSync.
        glXSwapIntervalEXT(display, glx_window, vsync);

        if(xi_available)
        {
          XIEventMask evmasks[2];
          u8 mask1[(XI_LASTEVENT + 7) / 8] = {0};
          u8 mask2[(XI_LASTEVENT + 7) / 8] = {0};

          // XISetMask(mask1, XI_KeyPress);
          // XISetMask(mask1, XI_KeyRelease);

          evmasks[0].deviceid = XIAllMasterDevices;
          evmasks[0].mask_len = sizeof(mask1);
          evmasks[0].mask = mask1;

          XISetMask(mask2, XI_ButtonPress);
          XISetMask(mask2, XI_ButtonRelease);
          XISetMask(mask2, XI_Motion);
          XISetMask(mask2, XI_DeviceChanged);

          evmasks[1].deviceid = 2;  // 2: Master pointer.
          evmasks[1].mask_len = sizeof(mask2);
          evmasks[1].mask = mask2;

          XISelectEvents(display, window, evmasks, 2);

          XIEventMask root_evmask;
          XISetMask(mask1, XI_RawMotion);
          root_evmask.deviceid = 2;
          root_evmask.mask_len = sizeof(mask1);
          root_evmask.mask = mask1;
          XISelectEvents(display, root_window, &root_evmask, 1);
        }

        set_title(display, window, (unsigned char*)PROGRAM_NAME, sizeof(PROGRAM_NAME) - 1);
        XMapWindow(display, window);

        glXMakeContextCurrent(display, glx_window, glx_window, glx_context);

        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
        // glBlendFunc(GL_ONE, GL_ONE_MINUS_SRC_ALPHA);

        Cursor empty_cursor;
        {
          char zero = 0;
          Pixmap empty_pixmap = XCreateBitmapFromData(display, root_window, &zero, 1, 1);
          XColor empty_color = {0};
          empty_cursor = XCreatePixmapCursor(display, empty_pixmap, empty_pixmap,
              &empty_color, &empty_color, 0, 0);
          XFreePixmap(display, empty_pixmap);
        }

        i32 img_count = max(1, argc - 1);
        img_entry_t* img_entries = malloc_array(img_count, img_entry_t);
        zero_bytes(img_count * sizeof(img_entry_t), img_entries);

        u8 test_texels[] = {
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
        GLuint test_texture_id = 0;

        for(i32 img_idx = 0;
            img_idx < img_count;
            ++img_idx)
        {
          img_entry_t* img = &img_entries[img_idx];

          if(argc > 1)
          {
            img->path = argv[img_idx + 1];
            img->pixels = stbi_load(img->path, &img->w, &img->h, 0, 4);
          }
          else
          {
            img->path = "<TEST IMAGE>";
          }

          if(!img->pixels)
          {
            img->pixels = test_texels;
            img->w = 3;
            img->h = 3;
            if(!test_texture_id)
            {
              glGenTextures(1, &test_texture_id);
            }
            img->texture_id = test_texture_id;
          }
          else
          {
            glGenTextures(1, &img->texture_id);
          }

          glBindTexture(GL_TEXTURE_2D, img->texture_id);
#if 0
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
#else
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_BORDER);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_BORDER);
#endif
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
          glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
          glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
              img->w, img->h, 0, GL_RGBA, GL_UNSIGNED_BYTE, img->pixels);
          glGenerateMipmap(GL_TEXTURE_2D);

#if 0
          // Premultiply alpha.
          for(u64 i = 0; i < (u64)img->w * (u64)img->h; ++i)
          {
            if(img->pixels[4*i + 3] != 255)
            {
              r32 r = img->pixels[4*i + 0] / 255.0f;
              r32 g = img->pixels[4*i + 1] / 255.0f;
              r32 b = img->pixels[4*i + 2] / 255.0f;
              r32 a = img->pixels[4*i + 3] / 255.0f;

              img->pixels[4*i + 0] = 255.0f * a * r;
              img->pixels[4*i + 1] = 255.0f * a * g;
              img->pixels[4*i + 2] = 255.0f * a * b;
            }
          }
#endif
        }

        i32 win_w = WINDOW_INIT_W;
        i32 win_h = WINDOW_INIT_H;
        GLuint texture_id = 0;

        r32 time = 0;
        u32 frames_since_last_print = 0;
        u64 nsecs_last_print = get_nanoseconds();
        u64 nsecs_last_frame = nsecs_last_print;
        i64 nsecs_min = I64_MAX;
        i64 nsecs_max = I64_MIN;

        b32 quitting = false;
        i32 viewing_img_idx = 0;
        i32 last_viewing_img_idx = -1;
        b32 border_sampling = true;
        b32 linear_sampling = true;
        b32 alpha_blend = true;
        b32 bright_bg = false;
        b32 extra_toggles[10] = {0};
        r32 zoom = 0;
        r32 offset_x = 0;
        r32 offset_y = 0;
        b32 hide_sidebar = false;
        i32 sidebar_width = 310;
        r32 sidebar_scroll_rows = 0;
        r32 thumbnail_w = 150.0f;
        r32 thumbnail_h = thumbnail_w;
        i32 hovered_thumbnail_idx = -1;

        // TODO: Split these for x/y ?
        //       Actually, just using the constant 120 might be enough.
        //       Check if any device is configured differently, ever.
        r32 default_scroll_increment = 120.0f;
        r32 scroll_increment_by_source_id[64];
        for(i32 i = 0; i < array_count(scroll_increment_by_source_id); ++i)
        { scroll_increment_by_source_id[i] = default_scroll_increment; }

        r32 offset_scroll_scale = 0.125f;
        r32 zoom_scroll_scale = 0.25f;

        r32 zoom_start_x = 0;
        r32 zoom_start_y = 0;
        r32 prev_mouse_x = 0;
        r32 prev_mouse_y = 0;
        b32 lmb_held = false;
        b32 mmb_held = false;
        b32 rmb_held = false;
        b32 shift_held = false;
        b32 ctrl_held = false;
        b32 alt_held = false;
        b32 has_focus = false;

        while(!quitting)
        {
          if(vsync)
          {
            // TODO: Check for this in GLX extension string before using it.
            glXDelayBeforeSwapNV(display, glx_window, 0.002f);
          }

          b32 scroll_thumbnail_into_view = false;

          while(XPending(display))
          {
            r32 mouse_x = prev_mouse_x;
            r32 mouse_y = prev_mouse_y;
            r32 mouse_delta_x = 0;
            r32 mouse_delta_y = 0;
            i32 scroll_y_ticks = 0;
            r32 scroll_x = 0;
            r32 scroll_y = 0;
            i32 mouse_btn_went_down = 0;
            i32 thumbnail_columns = max(1, (i32)((r32)sidebar_width / thumbnail_w));

            XEvent event;
            XNextEvent(display, &event);

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
                    lmb_held = (button_mask & 2);
                    mmb_held = (button_mask & 4);
                    rmb_held = (button_mask & 8);
                    mouse_x = (r32)devev->event_x;
                    mouse_y = (r32)win_h - (r32)devev->event_y - 1.0f;

#if 0
                    printf("%s %d:%d detail %d flags %d mods %d r %.2f,%.2f e %.2f,%.2f btns %d\n",
                        event.xcookie.evtype == XI_ButtonPress ? "XI_ButtonPress"
                        : event.xcookie.evtype == XI_ButtonRelease ? "XI_ButtonRelease" : "XI_Motion",
                        devev->deviceid, devev->sourceid,
                        devev->detail, devev->flags,
                        devev->mods.effective,
                        devev->root_x, devev->root_y, devev->event_x, devev->event_y,
                        button_mask);
                    printf("  valuators: mask_len %d mask 0x", devev->valuators.mask_len);
                    for_count(i, devev->valuators.mask_len) { printf("%02x", devev->valuators.mask[i]); }
                    printf("\n");
                    r64* values = devev->valuators.values;
                    for_count(i, devev->valuators.mask_len)
                    {
                      for_count(b, 8)
                      {
                        if(devev->valuators.mask[i] & (1 << b))
                        {
                          u32 idx = 8 * i + b;
                          printf("    %u: %.2f\n", idx, *values);
                          ++values;
                        }
                      }
                    }
#endif

                    if(event.xcookie.evtype == XI_ButtonPress)
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

                      zoom_start_x = mouse_x;
                      zoom_start_y = mouse_y;
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

                      r32 scroll_increment =
                        devev->sourceid < array_count(scroll_increment_by_source_id)
                        ? scroll_increment_by_source_id[devev->sourceid]
                        : default_scroll_increment;

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
                              else if(idx == 2)
                              {
                                scroll_x += value / scroll_increment;
                              }
                              else if(idx == 3)
                              {
                                scroll_y -= value / scroll_increment;
                              }
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
                    // printf("XI Device %d changed\n", device->deviceid);
                    update_scroll_increments(device->num_classes, device->classes,
                        array_count(scroll_increment_by_source_id), scroll_increment_by_source_id);
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
                    if(keysym == XK_Escape)
                    {
                      quitting = true;
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
                    else if(keysym == XK_BackSpace || (alt_held && keysym == XK_Left))
                    {
                      viewing_img_idx -= 1;
                    }
                    else if(keysym == ' ' || (alt_held && keysym == XK_Right))
                    {
                      viewing_img_idx += 1;
                    }
                    else if(alt_held && keysym == XK_Up)
                    {
                      viewing_img_idx -= thumbnail_columns;
                    }
                    else if(alt_held && keysym == XK_Down)
                    {
                      viewing_img_idx += thumbnail_columns;
                    }
                    else if(keysym == XK_Home)
                    {
                      viewing_img_idx = 0;
                    }
                    else if(keysym == XK_End)
                    {
                      viewing_img_idx = img_count - 1;
                    }
                    else if(keysym == 'a')
                    {
                      bflip(border_sampling);
                    }
                    else if(keysym == 'b')
                    {
                      bflip(bright_bg);
                    }
                    else if(keysym == 'h')
                    {
                      bflip(hide_sidebar);
                    }
                    else if(keysym == 'l')
                    {
                      bflip(linear_sampling);
                      for(i32 img_idx = 0;
                          img_idx < img_count;
                          ++img_idx)
                      {
                        glBindTexture(GL_TEXTURE_2D, img_entries[img_idx].texture_id);
                        if(linear_sampling)
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
                    else if(keysym == 't')
                    {
                      bflip(alpha_blend);
                    }
                    else if(keysym == 'v')
                    {
                      bflip(vsync);

                      glXSwapIntervalEXT(display, glx_window, (int)vsync);
                    }
                    else if(keysym == 'x')
                    {
                      zoom = 0;
                      offset_x = 0;
                      offset_y = 0;
                    }
                    else if(keysym == '0')
                    {
                      zoom = 0;
                    }
                    else if(keysym == '-')
                    {
                      zoom -= 0.25f;
                    }
                    else if(keysym == '=')
                    {
                      zoom += 0.25f;
                    }
                    else if(keysym >= '0' && keysym <= '9')
                    {
                      bflip(extra_toggles[keysym - '0']);
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
                  mouse_y = win_h - event.xbutton.y - 1;

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

                    zoom_start_x = mouse_x;
                    zoom_start_y = mouse_y;
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
                  mouse_y = win_h - event.xbutton.y - 1;

                  mouse_delta_x += mouse_x - prev_mouse_x;
                  mouse_delta_y += mouse_y - prev_mouse_y;
                } break;

                case FocusIn:
                {
                  has_focus = true;

                  int device_count = 0;
                  int master_pointer_device_id = 2;
                  XIDeviceInfo* device_infos = XIQueryDevice(display, master_pointer_device_id, &device_count);
                  // printf("\nDevices:\n");
                  for(i32 device_idx = 0;
                      device_idx < device_count;
                      ++device_idx)
                  {
                    XIDeviceInfo* device = &device_infos[device_idx];
                    // printf("  deviceid: %d\n", device->deviceid);
                    // printf("  name: %s\n", device->name);
                    update_scroll_increments(device->num_classes, device->classes,
                        array_count(scroll_increment_by_source_id), scroll_increment_by_source_id);
                  }
                } break;

                case FocusOut:
                {
                  has_focus = false;
                } break;

                case ConfigureNotify:
                {
                  win_w = event.xconfigure.width;
                  win_h = event.xconfigure.height;

                  mouse_x = 0.5f * (r32)win_w;
                  mouse_y = 0.5f * (r32)win_h;
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

                default:
                {
                  // printf("Unhandled event type=%u\n", event.type);
                } break;
              }
            }

            // Handle mouse events.
            b32 mouse_in_sidebar = (!hide_sidebar && mouse_x < sidebar_width);
            if(mouse_in_sidebar && hovered_thumbnail_idx != -1 && (mouse_btn_went_down == 1 || lmb_held))
            {
              viewing_img_idx = hovered_thumbnail_idx;
            }
            else if(mouse_delta_x || mouse_delta_y || scroll_x || scroll_y || scroll_y_ticks || mouse_btn_went_down)
            {
              r32 win_min_side = min(win_w, win_h);
              r32 exp_zoom_before = exp2f(zoom);
              r32 offset_per_scroll = offset_scroll_scale / exp_zoom_before;
              r32 zoom_delta = 0;

              if(alt_held)
              {
                viewing_img_idx -= scroll_y_ticks;
              }

              if(!alt_held)
              {
                if(mouse_in_sidebar)
                {
                  if(ctrl_held)
                  {
                    if(scroll_y != 0)
                    {
                      thumbnail_w *= exp2f(scroll_y);
                      thumbnail_h = thumbnail_w;
                      scroll_thumbnail_into_view = true;
                    }
                  }
                  else if(shift_held)
                  {
                  }
                  else
                  {
                    sidebar_scroll_rows -= scroll_y;
                    sidebar_scroll_rows = clamp(0,
                        (img_count + thumbnail_columns - 1) / thumbnail_columns - 1,
                        sidebar_scroll_rows);
                  }
                }
                else
                {
                  if(ctrl_held)
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
                zoom_start_x = mouse_x;
                zoom_start_y = mouse_y;
              }

              if(lmb_held || mmb_held)
              {
                if(ctrl_held || mmb_held)
                {
                  zoom_delta += 4.0f * mouse_delta_y / win_min_side;
                }
                else
                {
                  offset_x += mouse_delta_x / (exp_zoom_before * win_min_side);
                  offset_y += mouse_delta_y / (exp_zoom_before * win_min_side);

                  zoom_start_x = mouse_x;
                  zoom_start_y = mouse_y;
                }
              }

              if(zoom_delta != 0.0f)
              {
                zoom += zoom_delta;
                r32 exp_zoom_after = exp2f(zoom);

                r32 center_mouse_x = zoom_start_x - 0.5f * win_w;
                r32 center_mouse_y = zoom_start_y - 0.5f * win_h;

                if(!hide_sidebar)
                {
                  center_mouse_x -= 0.5f * (r32)sidebar_width;
                }

                offset_x += center_mouse_x / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
                offset_y += center_mouse_y / win_min_side * (1.0f / exp_zoom_after - 1.0f / exp_zoom_before);
              }
            }

            prev_mouse_x = mouse_x;
            prev_mouse_y = mouse_y;
          }
          i32 thumbnail_columns = max(1, (i32)((r32)sidebar_width / thumbnail_w));

          viewing_img_idx = i32_wrap_upto(viewing_img_idx, img_count);
          if(last_viewing_img_idx != viewing_img_idx)
          {
            char txt[256];
            txt[0] = 0;
            i32 txt_len = snprintf(txt, sizeof(txt), "%s [%d/%d] %dx%d %s",
                PROGRAM_NAME,
                viewing_img_idx + 1, img_count,
                img_entries[viewing_img_idx].w, img_entries[viewing_img_idx].h,
                img_entries[viewing_img_idx].path);
            set_title(display, window, (u8*)txt, txt_len);

            scroll_thumbnail_into_view = true;

            last_viewing_img_idx = viewing_img_idx;
          }

          if(scroll_thumbnail_into_view)
          {
            i32 thumbnail_row = viewing_img_idx / thumbnail_columns;
            if((thumbnail_row - sidebar_scroll_rows + 1) * thumbnail_h > win_h)
            {
              sidebar_scroll_rows = thumbnail_row + 1 - win_h / thumbnail_h;
            }
            if(thumbnail_row - sidebar_scroll_rows < 0)
            {
              sidebar_scroll_rows = thumbnail_row;
            }
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

          glViewport(0, 0, win_w, win_h);
          glDisable(GL_SCISSOR_TEST);
          {
            r32 bg_gray = bright_bg ? 0.9f : 0.1f;
            glClearColor(bg_gray, bg_gray, bg_gray, 1.0f);
          }
          glClear(GL_COLOR_BUFFER_BIT);

          if(win_w != 0 && win_h != 0)
          {
            glMatrixMode(GL_PROJECTION);
            r32 matrix[16] = { // Column-major!
              2.0f / win_w, 0.0f, 0.0f, 0.0f,
              0.0f, 2.0f / win_h, 0.0f, 0.0f,
              0.0f, 0.0f, 1.0f, 0.0f,
              -1.0f, -1.0f, 0.0f, 1.0f,
            };
            glLoadMatrixf(matrix);
          }

          if(!hide_sidebar)
          {
            glEnable(GL_SCISSOR_TEST);
            glScissor(0, 0, sidebar_width, win_h);

            glDisable(GL_BLEND);
            glDisable(GL_TEXTURE_2D);
            glBegin(GL_QUADS);
            if(bright_bg)
            {
              glColor3f(0.0f, 0.0f, 0.0f);
            }
            else
            {
              glColor3f(1.0f, 1.0f, 1.0f);
            }
            glVertex2f(sidebar_width - 2, 0);
            glVertex2f(sidebar_width - 1, 0);
            glVertex2f(sidebar_width - 1, win_h);
            glVertex2f(sidebar_width - 2, win_h);
            glEnd();

            glScissor(0, 0, sidebar_width - 2, win_h);

            if(alpha_blend)
            {
              glEnable(GL_BLEND);
            }
            else
            {
              glDisable(GL_BLEND);
            }

            glEnable(GL_TEXTURE_2D);
            glColor3f(1.0f, 1.0f, 1.0f);
            hovered_thumbnail_idx = -1;
            for(i32 img_idx = 0;
                img_idx < img_count;
                ++img_idx)
            {
              img_entry_t* img = &img_entries[img_idx];
              if(img->texture_id)
              {
                r32 tex_w = img->w;
                r32 tex_h = img->h;

                glBindTexture(GL_TEXTURE_2D, img->texture_id);

                i32 sidebar_col = img_idx % thumbnail_columns;
                i32 sidebar_row = img_idx / thumbnail_columns;

                r32 box_x0 = sidebar_col * thumbnail_w;
                r32 box_y1 = win_h - ((r32)sidebar_row - sidebar_scroll_rows) * thumbnail_h;
                r32 box_x1 = box_x0 + thumbnail_w;
                r32 box_y0 = box_y1 - thumbnail_h;

                if(hovered_thumbnail_idx == -1 &&
                    prev_mouse_x >= box_x0 && prev_mouse_x < box_x1 &&
                    prev_mouse_y >= box_y0 && prev_mouse_y < box_y1)
                {
                  hovered_thumbnail_idx = img_idx;
                }

                if(img_idx == viewing_img_idx || img_idx == hovered_thumbnail_idx)
                {
                  glDisable(GL_TEXTURE_2D);

                  r32 gray = (img_idx == viewing_img_idx) ? (bright_bg ? 0.3f : 0.7f) : 0.5f;
                  glColor3f(gray, gray, gray);

                  glBegin(GL_QUADS);
                  glVertex2f(box_x0, box_y0);
                  glVertex2f(box_x1, box_y0);
                  glVertex2f(box_x1, box_y1);
                  glVertex2f(box_x0, box_y1);
                  glEnd();

                  glEnable(GL_TEXTURE_2D);
                  glColor3f(1.0f, 1.0f, 1.0f);
                }

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
            }

            glScissor(sidebar_width, 0, win_w - sidebar_width, win_h);
          }

          img_entry_t* img = &img_entries[viewing_img_idx];

          glBindTexture(GL_TEXTURE_2D, img->texture_id);

          r32 tex_w = img->w;
          r32 tex_h = img->h;

          if(alpha_blend)
          {
            glEnable(GL_BLEND);
          }
          else
          {
            glDisable(GL_BLEND);
          }

          r32 u0 = 0.0f;
          r32 v0 = 0.0f;
          r32 u1 = 1.0f;
          r32 v1 = 1.0f;

          r32 image_region_x0 = hide_sidebar ? 0 : sidebar_width;
          r32 image_region_w = (r32)win_w - image_region_x0;

          r32 mag = 1.0f;
          if(image_region_w != 0 && win_h != 0)
          {
            mag = min(image_region_w / (r32)tex_w, (r32)win_h / (r32)tex_h);
          }
          r32 exp_zoom = exp2f(zoom);
          mag *= exp_zoom;

          if(absolute(mag - 1.0f) <= 1e-3f)
          {
            mag = 1.0f;
          }

          r32 x0 = 0.5f * (image_region_w - mag * tex_w) + image_region_x0;
          r32 y0 = 0.5f * (win_h - mag * tex_h);

          r32 win_min_side = min(win_w, win_h);
          x0 += win_min_side * exp_zoom * offset_x;
          y0 += win_min_side * exp_zoom * offset_y;

          if(mag == 1.0f)
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
          if(vsync)
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

          u64 nsecs_now = get_nanoseconds();
          i64 nsecs = nsecs_now - nsecs_last_frame;
          nsecs_last_frame = nsecs_now;
#if 1
          if(!vsync || extra_toggles[5])
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
